#include "StreamResolver.h"

#include "DashParser.h"
#include "HlsParser.h"
#include "core/UrlValidator.h"

#include <QCryptographicHash>
#include <QHash>
#include <QHostAddress>

#include <algorithm>

namespace haul {

namespace {

constexpr qint64 kMaxManifestSize = 10 * 1024 * 1024;

struct Fetched {
    bool ok = false;
    TransferError error;
    QByteArray body;
    QUrl url; // after redirects
};

struct FetchContext {
    QByteArray body;
    bool tooLarge = false;
    const std::atomic_bool *cancel = nullptr;
};

size_t fetchWrite(char *data, size_t size, size_t count, void *userData)
{
    auto *context = static_cast<FetchContext *>(userData);
    if (context->body.size() + static_cast<qint64>(size * count) > kMaxManifestSize) {
        context->tooLarge = true;
        return 0;
    }
    context->body.append(data, static_cast<qsizetype>(size * count));
    return size * count;
}

int fetchProgress(void *userData, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
{
    auto *context = static_cast<FetchContext *>(userData);
    return context->cancel && context->cancel->load() ? 1 : 0;
}

Fetched fetchManifest(const QUrl &url, const NetworkOptions &options, const std::atomic_bool *cancel)
{
    Fetched result;
    CurlEasy curl;
    if (!curl.isValid()) {
        result.error = { ErrorKind::Permanent, QStringLiteral("Could not initialise libcurl."), 0, -1 };
        return result;
    }
    FetchContext context;
    context.cancel = cancel;
    curl.configure(url, options);
    curl.applyHeaders();
    curl_easy_setopt(curl.handle(), CURLOPT_WRITEFUNCTION, &fetchWrite);
    curl_easy_setopt(curl.handle(), CURLOPT_WRITEDATA, &context);
    curl_easy_setopt(curl.handle(), CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl.handle(), CURLOPT_XFERINFOFUNCTION, &fetchProgress);
    curl_easy_setopt(curl.handle(), CURLOPT_XFERINFODATA, &context);
    curl_easy_setopt(curl.handle(), CURLOPT_TIMEOUT, 60L);

    const CURLcode code = curl_easy_perform(curl.handle());
    if (context.tooLarge) {
        result.error = { ErrorKind::Permanent, QStringLiteral("This is too large to be a video manifest."), 0, -1 };
        return result;
    }
    if (code != CURLE_OK) {
        result.error = errorFromCurl(code, curl.errorBuffer());
        return result;
    }
    const int status = curl.response().status;
    const ErrorKind kind = classifyHttpStatus(status);
    if (kind != ErrorKind::None) {
        result.error = { kind, describeHttpStatus(status), status,
                         parseRetryAfter(curl.response().value(QStringLiteral("retry-after"))) };
        return result;
    }
    result.ok = true;
    result.body = context.body;
    result.url = QUrl(curl.effectiveUrl());
    if (!result.url.isValid() || result.url.isEmpty())
        result.url = url;
    return result;
}

TransferError permanent(const QString &message)
{
    return { ErrorKind::Permanent, message, 0, -1 };
}

TransferError encryptedError()
{
    return permanent(QString::fromLatin1(kEncryptedStreamMessage));
}

// A page on the internet must not be able to point Haul at machines on the user's own network.
bool segmentsAreAllowed(const QVector<MediaSegment> &segments, const QUrl &manifest, QString *error)
{
    const bool manifestIsLocal = isLocalNetworkHost(manifest.host());
    for (const MediaSegment &segment : segments) {
        const QUrl url(segment.url);
        const QString scheme = url.scheme().toLower();
        if ((scheme != QLatin1String("http") && scheme != QLatin1String("https")) || url.host().isEmpty()) {
            *error = QStringLiteral("The manifest points to an unsupported address.");
            return false;
        }
        if (!manifestIsLocal && isLocalNetworkHost(url.host())) {
            *error = QStringLiteral("The manifest points to a device on your local network, so Haul refused it.");
            return false;
        }
    }
    return true;
}

QString fingerprintOf(const QVector<StreamTrack> &tracks)
{
    // Durations and counts, not URLs: CDNs often change URL tokens on every fetch.
    QByteArray text;
    for (const StreamTrack &track : tracks) {
        text += QByteArray::number(static_cast<int>(track.role)) + ':' + QByteArray::number(track.segments.size()) + ':';
        for (const MediaSegment &segment : track.segments)
            text += QByteArray::number(qRound64(segment.duration * 1000)) + ',';
    }
    return QString::fromLatin1(QCryptographicHash::hash(text, QCryptographicHash::Sha256).toHex());
}

const StreamVariant *chooseVariant(const QVector<StreamVariant> &variants, const QString &id)
{
    if (id.isEmpty())
        return variants.isEmpty() ? nullptr : &variants.first();
    for (const StreamVariant &variant : variants) {
        if (variant.id == id)
            return &variant;
    }
    return nullptr;
}

bool loadHlsMedia(const QUrl &url, const NetworkOptions &options, const std::atomic_bool *cancel, HlsMedia *media, TransferError *error)
{
    const Fetched fetched = fetchManifest(url, options, cancel);
    if (!fetched.ok) {
        *error = fetched.error;
        return false;
    }
    QString message;
    if (!parseHlsMedia(fetched.body, fetched.url, media, &message)) {
        *error = permanent(message);
        return false;
    }
    if (media->encrypted) {
        *error = encryptedError();
        return false;
    }
    if (media->live) {
        *error = permanent(QString::fromLatin1(kLiveStreamMessage));
        return false;
    }
    if (!segmentsAreAllowed(media->segments, fetched.url, &message)) {
        *error = permanent(message);
        return false;
    }
    return true;
}

double mediaDuration(const HlsMedia &media)
{
    double total = 0.0;
    for (const MediaSegment &segment : media.segments)
        total += segment.duration;
    return total;
}

// A DASH quality is one video representation, plus the best audio if it has none of its own.
QVector<StreamVariant> dashVariants(const DashManifest &manifest, const DashRepresentation **bestAudio)
{
    QVector<StreamVariant> variants;
    *bestAudio = nullptr;
    for (const DashRepresentation &rep : manifest.representations) {
        if (!rep.isVideo && !rep.encrypted && (!*bestAudio || rep.bandwidth > (*bestAudio)->bandwidth))
            *bestAudio = &rep;
    }
    for (const DashRepresentation &rep : manifest.representations) {
        if (!rep.isVideo || rep.encrypted)
            continue;
        StreamVariant variant;
        variant.id = rep.id;
        variant.width = rep.width;
        variant.height = rep.height;
        variant.bandwidth = rep.bandwidth + (!rep.isMuxed && *bestAudio ? (*bestAudio)->bandwidth : 0);
        if (!rep.isMuxed) {
            variant.videoBandwidth = rep.bandwidth;
            variant.audioBandwidth = *bestAudio ? (*bestAudio)->bandwidth : 0;
        }
        variant.codecs = rep.codecs;
        variant.separateAudio = !rep.isMuxed && *bestAudio;
        variant.estimatedBytes = manifest.duration > 0 ? static_cast<qint64>(variant.bandwidth * manifest.duration / 8) : -1;
        variants.append(variant);
    }
    if (variants.isEmpty() && *bestAudio) {
        StreamVariant variant;
        variant.id = (*bestAudio)->id;
        variant.bandwidth = (*bestAudio)->bandwidth;
        variant.codecs = (*bestAudio)->codecs;
        variant.audioOnly = true;
        variant.audioBandwidth = variant.bandwidth;
        variant.estimatedBytes = manifest.duration > 0 ? static_cast<qint64>(variant.bandwidth * manifest.duration / 8) : -1;
        variants.append(variant);
    }
    std::stable_sort(variants.begin(), variants.end(), [](const StreamVariant &a, const StreamVariant &b) {
        if (a.height != b.height)
            return a.height > b.height;
        return a.bandwidth > b.bandwidth;
    });
    return variants;
}

} // namespace

static void nameVariants(QVector<StreamVariant> &variants)
{
    QHash<QString, int> counts;
    for (const StreamVariant &variant : variants)
        ++counts[variant.label()];
    for (StreamVariant &variant : variants) {
        variant.displayName = variant.label();
        if (counts.value(variant.label()) > 1 && variant.bandwidth > 0)
            variant.displayName += QStringLiteral(" (%1 kbps)").arg(variant.bandwidth / 1000);
    }
}

StreamKind streamKindForUrl(const QUrl &url)
{
    const QString path = url.path().toLower();
    if (path.endsWith(QLatin1String(".m3u8")) || path.endsWith(QLatin1String(".m3u")))
        return StreamKind::Hls;
    if (path.endsWith(QLatin1String(".mpd")))
        return StreamKind::Dash;
    return StreamKind::File;
}

QString streamKindKey(StreamKind kind)
{
    switch (kind) {
    case StreamKind::Hls: return QStringLiteral("hls");
    case StreamKind::Dash: return QStringLiteral("dash");
    case StreamKind::File: break;
    }
    return QStringLiteral("file");
}

StreamKind streamKindFromKey(const QString &key)
{
    if (key == QLatin1String("hls"))
        return StreamKind::Hls;
    if (key == QLatin1String("dash"))
        return StreamKind::Dash;
    return StreamKind::File;
}

bool isLocalNetworkHost(const QString &host)
{
    const QString lower = host.toLower();
    if (lower == QLatin1String("localhost") || lower.endsWith(QLatin1String(".localhost")) || lower.endsWith(QLatin1String(".local")))
        return true;
    const QHostAddress address(host);
    if (address.isNull())
        return false;
    return address.isLoopback() || address.isLinkLocal() || address.isSiteLocal() || address.isUniqueLocalUnicast()
        || address.isInSubnet(QHostAddress(QStringLiteral("10.0.0.0")), 8)
        || address.isInSubnet(QHostAddress(QStringLiteral("172.16.0.0")), 12)
        || address.isInSubnet(QHostAddress(QStringLiteral("192.168.0.0")), 16)
        || address.isInSubnet(QHostAddress(QStringLiteral("169.254.0.0")), 16);
}

StreamInfo inspectStream(const QUrl &url, const NetworkOptions &options, const std::atomic_bool *cancel)
{
    StreamInfo info;
    const Fetched fetched = fetchManifest(url, options, cancel);
    if (!fetched.ok) {
        info.error = fetched.error;
        return info;
    }

    QString message;
    if (isHlsPlaylist(fetched.body)) {
        info.kind = StreamKind::Hls;
        if (!isHlsMaster(fetched.body)) {
            HlsMedia media;
            if (!loadHlsMedia(url, options, cancel, &media, &info.error))
                return info;
            StreamVariant only;
            only.id = QStringLiteral("default");
            only.displayName = only.label();
            info.duration = mediaDuration(media);
            info.variants.append(only);
            info.ok = true;
            return info;
        }

        HlsMaster master;
        if (!parseHlsMaster(fetched.body, fetched.url, &master, &message)) {
            info.error = permanent(message);
            return info;
        }
        if (master.sessionEncrypted) {
            info.error = encryptedError();
            return info;
        }
        for (const StreamVariant &variant : master.variants)
            info.childUrls.append(variant.url);
        info.childUrls += master.audioGroups.values();

        // Variants normally share one length, so reading the best one is enough for size estimates
        // and tells us whether the stream is encrypted or live.
        HlsMedia media;
        if (!loadHlsMedia(QUrl(master.variants.first().url), options, cancel, &media, &info.error))
            return info;
        info.duration = mediaDuration(media);
        for (StreamVariant &variant : master.variants) {
            if (variant.bandwidth > 0 && info.duration > 0)
                variant.estimatedBytes = static_cast<qint64>(variant.bandwidth * info.duration / 8);
        }
        info.variants = master.variants;
        nameVariants(info.variants);
        info.ok = true;
        return info;
    }

    if (isDashManifest(fetched.body)) {
        info.kind = StreamKind::Dash;
        DashManifest manifest;
        if (!parseDashManifest(fetched.body, fetched.url, &manifest, &message)) {
            info.error = permanent(message);
            return info;
        }
        if (manifest.live) {
            info.error = permanent(QString::fromLatin1(kLiveStreamMessage));
            return info;
        }
        const DashRepresentation *audio = nullptr;
        info.variants = dashVariants(manifest, &audio);
        if (info.variants.isEmpty()) {
            info.error = encryptedError(); // every representation carried ContentProtection
            return info;
        }
        nameVariants(info.variants);
        info.duration = manifest.duration;
        info.ok = true;
        return info;
    }

    // Not a manifest: treat it as an ordinary file.
    info.kind = StreamKind::File;
    if (streamKindForUrl(url) != StreamKind::File)
        info.error = permanent(QStringLiteral("This address doesn't contain a valid video manifest."));
    else
        info.ok = true;
    return info;
}

StreamPlan resolveStreamPlan(const QUrl &url, StreamKind kind, const QString &variantId, const NetworkOptions &options,
                             const std::atomic_bool *cancel)
{
    StreamPlan plan;
    plan.kind = kind;
    const Fetched fetched = fetchManifest(url, options, cancel);
    if (!fetched.ok) {
        plan.error = fetched.error;
        return plan;
    }

    QString message;
    const TransferError gone = permanent(QStringLiteral("The quality you chose is no longer offered. Add the video again."));

    if (kind == StreamKind::Hls) {
        if (!isHlsPlaylist(fetched.body)) {
            plan.error = permanent(QStringLiteral("This is not a valid HLS playlist."));
            return plan;
        }
        HlsMedia media;
        StreamTrack main;
        main.role = StreamTrack::Muxed;
        if (isHlsMaster(fetched.body)) {
            HlsMaster master;
            if (!parseHlsMaster(fetched.body, fetched.url, &master, &message)) {
                plan.error = permanent(message);
                return plan;
            }
            if (master.sessionEncrypted) {
                plan.error = encryptedError();
                return plan;
            }
            const StreamVariant *variant = chooseVariant(master.variants, variantId);
            if (!variant) {
                plan.error = gone;
                return plan;
            }
            plan.label = variant->label();
            if (!loadHlsMedia(QUrl(variant->url), options, cancel, &media, &plan.error))
                return plan;
            main.extension = media.extension;
            main.segments = media.segments;
            plan.estimatedBytes = variant->bandwidth > 0 ? static_cast<qint64>(variant->bandwidth * mediaDuration(media) / 8) : -1;
            if (variant->separateAudio) {
                main.role = StreamTrack::Video;
                HlsMedia audioMedia;
                if (!loadHlsMedia(QUrl(master.audioGroups.value(variant->audioGroup)), options, cancel, &audioMedia, &plan.error))
                    return plan;
                StreamTrack audio;
                audio.role = StreamTrack::Audio;
                audio.extension = audioMedia.extension == QLatin1String("mp4") ? QStringLiteral("m4a") : audioMedia.extension;
                audio.segments = audioMedia.segments;
                plan.tracks.append(main);
                plan.tracks.append(audio);
            }
        } else {
            if (!loadHlsMedia(url, options, cancel, &media, &plan.error))
                return plan;
            main.extension = media.extension;
            main.segments = media.segments;
        }
        if (plan.tracks.isEmpty())
            plan.tracks.append(main);
    } else {
        if (!isDashManifest(fetched.body)) {
            plan.error = permanent(QStringLiteral("This is not a valid DASH manifest."));
            return plan;
        }
        DashManifest manifest;
        if (!parseDashManifest(fetched.body, fetched.url, &manifest, &message)) {
            plan.error = permanent(message);
            return plan;
        }
        if (manifest.live) {
            plan.error = permanent(QString::fromLatin1(kLiveStreamMessage));
            return plan;
        }
        const DashRepresentation *audio = nullptr;
        const QVector<StreamVariant> variants = dashVariants(manifest, &audio);
        if (variants.isEmpty()) {
            plan.error = encryptedError();
            return plan;
        }
        const StreamVariant *variant = chooseVariant(variants, variantId);
        if (!variant) {
            plan.error = gone;
            return plan;
        }
        plan.label = variant->label();
        plan.estimatedBytes = variant->estimatedBytes;
        for (const DashRepresentation &rep : manifest.representations) {
            if (rep.id != variant->id || rep.encrypted)
                continue;
            StreamTrack track;
            track.role = variant->audioOnly ? StreamTrack::Audio : (variant->separateAudio ? StreamTrack::Video : StreamTrack::Muxed);
            track.extension = rep.extension;
            track.segments = rep.segments;
            plan.tracks.append(track);
            break;
        }
        if (variant->separateAudio && audio) {
            StreamTrack track;
            track.role = StreamTrack::Audio;
            track.extension = audio->extension;
            track.segments = audio->segments;
            plan.tracks.append(track);
        }
        if (plan.tracks.isEmpty()) {
            plan.error = gone;
            return plan;
        }
    }

    for (const StreamTrack &track : std::as_const(plan.tracks)) {
        if (!segmentsAreAllowed(track.segments, fetched.url, &message)) {
            plan.error = permanent(message);
            return plan;
        }
    }
    plan.fingerprint = fingerprintOf(plan.tracks);
    plan.ok = true;
    return plan;
}

} // namespace haul
