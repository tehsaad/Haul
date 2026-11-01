#include "HlsParser.h"

#include <QFileInfo>
#include <QHash>

#include <algorithm>

namespace haul {

namespace {

// Parses NAME=VALUE,NAME="VALUE, with commas" attribute lists.
QHash<QString, QString> parseAttributes(const QString &text)
{
    QHash<QString, QString> attributes;
    int i = 0;
    while (i < text.size()) {
        const int eq = text.indexOf(QLatin1Char('='), i);
        if (eq < 0)
            break;
        const QString name = text.mid(i, eq - i).trimmed();
        i = eq + 1;
        QString value;
        if (i < text.size() && text[i] == QLatin1Char('"')) {
            const int close = text.indexOf(QLatin1Char('"'), i + 1);
            if (close < 0)
                break;
            value = text.mid(i + 1, close - i - 1);
            i = close + 1;
        } else {
            int comma = text.indexOf(QLatin1Char(','), i);
            if (comma < 0)
                comma = text.size();
            value = text.mid(i, comma - i);
            i = comma;
        }
        attributes.insert(name, value);
        if (i < text.size() && text[i] == QLatin1Char(','))
            ++i;
    }
    return attributes;
}

QStringList playlistLines(const QByteArray &body)
{
    QStringList lines = QString::fromUtf8(body).split(QLatin1Char('\n'));
    for (QString &line : lines)
        line = line.trimmed();
    return lines;
}

QString resolveUrl(const QUrl &base, const QString &reference)
{
    return base.resolved(QUrl(reference)).toString(QUrl::FullyEncoded);
}

bool isEncryptedKey(const QString &line)
{
    const int colon = line.indexOf(QLatin1Char(':'));
    const QHash<QString, QString> attributes = parseAttributes(line.mid(colon + 1));
    return attributes.value(QStringLiteral("METHOD")).compare(QLatin1String("NONE"), Qt::CaseInsensitive) != 0;
}

} // namespace

bool isHlsPlaylist(const QByteArray &body)
{
    QByteArray start = body.left(1024);
    if (start.startsWith("\xEF\xBB\xBF"))
        start = start.mid(3);
    return start.trimmed().startsWith("#EXTM3U");
}

bool isHlsMaster(const QByteArray &body)
{
    return body.contains("#EXT-X-STREAM-INF");
}

bool parseHlsMaster(const QByteArray &body, const QUrl &base, HlsMaster *master, QString *error)
{
    if (!isHlsPlaylist(body)) {
        *error = QStringLiteral("This is not a valid HLS playlist.");
        return false;
    }

    const QStringList lines = playlistLines(body);
    QHash<QString, QString> pending;
    bool expectingUri = false;
    for (const QString &line : lines) {
        if (line.isEmpty())
            continue;
        if (line.startsWith(QLatin1String("#EXT-X-SESSION-KEY"))) {
            if (isEncryptedKey(line))
                master->sessionEncrypted = true;
        } else if (line.startsWith(QLatin1String("#EXT-X-MEDIA:"))) {
            const auto attributes = parseAttributes(line.mid(13));
            const QString type = attributes.value(QStringLiteral("TYPE"));
            const QString uri = attributes.value(QStringLiteral("URI"));
            const QString group = attributes.value(QStringLiteral("GROUP-ID"));
            if (type == QLatin1String("AUDIO") && !uri.isEmpty()) {
                // The DEFAULT rendition wins; otherwise the first one listed.
                const bool isDefault = attributes.value(QStringLiteral("DEFAULT")) == QLatin1String("YES");
                if (isDefault || !master->audioGroups.contains(group))
                    master->audioGroups.insert(group, resolveUrl(base, uri));
            }
        } else if (line.startsWith(QLatin1String("#EXT-X-STREAM-INF:"))) {
            pending = parseAttributes(line.mid(18));
            expectingUri = true;
        } else if (!line.startsWith(QLatin1Char('#')) && expectingUri) {
            expectingUri = false;
            StreamVariant variant;
            variant.url = resolveUrl(base, line);
            variant.bandwidth = pending.value(QStringLiteral("BANDWIDTH")).toLongLong();
            variant.codecs = pending.value(QStringLiteral("CODECS"));
            variant.audioGroup = pending.value(QStringLiteral("AUDIO"));
            const QStringList resolution = pending.value(QStringLiteral("RESOLUTION")).split(QLatin1Char('x'));
            if (resolution.size() == 2) {
                variant.width = resolution[0].toInt();
                variant.height = resolution[1].toInt();
            }
            variant.audioOnly = variant.height == 0 && !variant.codecs.isEmpty()
                && !variant.codecs.contains(QLatin1String("avc")) && !variant.codecs.contains(QLatin1String("hvc"))
                && !variant.codecs.contains(QLatin1String("hev")) && !variant.codecs.contains(QLatin1String("vp"))
                && !variant.codecs.contains(QLatin1String("av01"));
            master->variants.append(variant);
        }
    }

    if (master->variants.isEmpty()) {
        *error = QStringLiteral("The playlist doesn't list any video.");
        return false;
    }
    for (StreamVariant &variant : master->variants) {
        variant.separateAudio = master->audioGroups.contains(variant.audioGroup);
        variant.id = QStringLiteral("%1p-%2").arg(variant.height).arg(variant.bandwidth);
    }
    std::stable_sort(master->variants.begin(), master->variants.end(), [](const StreamVariant &a, const StreamVariant &b) {
        if (a.audioOnly != b.audioOnly)
            return b.audioOnly;
        if (a.height != b.height)
            return a.height > b.height;
        return a.bandwidth > b.bandwidth;
    });
    return true;
}

bool parseHlsMedia(const QByteArray &body, const QUrl &base, HlsMedia *media, QString *error)
{
    if (!isHlsPlaylist(body)) {
        *error = QStringLiteral("This is not a valid HLS playlist.");
        return false;
    }

    media->live = true;
    double duration = -1.0;
    qint64 rangeLength = -1;
    qint64 rangeOffset = -1;
    qint64 nextOffset = 0;
    QString lastMap;
    const QStringList lines = playlistLines(body);
    for (const QString &line : lines) {
        if (line.isEmpty())
            continue;
        if (line.startsWith(QLatin1String("#EXTINF:"))) {
            duration = line.mid(8).section(QLatin1Char(','), 0, 0).toDouble();
        } else if (line.startsWith(QLatin1String("#EXT-X-BYTERANGE:"))) {
            const QString value = line.mid(17);
            rangeLength = value.section(QLatin1Char('@'), 0, 0).toLongLong();
            rangeOffset = value.contains(QLatin1Char('@')) ? value.section(QLatin1Char('@'), 1, 1).toLongLong() : -1;
        } else if (line.startsWith(QLatin1String("#EXT-X-ENDLIST"))) {
            media->live = false;
        } else if (line.startsWith(QLatin1String("#EXT-X-KEY:"))) {
            if (isEncryptedKey(line))
                media->encrypted = true;
        } else if (line.startsWith(QLatin1String("#EXT-X-MAP:"))) {
            const auto attributes = parseAttributes(line.mid(11));
            const QString uri = resolveUrl(base, attributes.value(QStringLiteral("URI")));
            const QString key = uri + attributes.value(QStringLiteral("BYTERANGE"));
            if (key != lastMap) {
                lastMap = key;
                MediaSegment init;
                init.url = uri;
                init.isInit = true;
                const QString range = attributes.value(QStringLiteral("BYTERANGE"));
                if (!range.isEmpty()) {
                    init.rangeLength = range.section(QLatin1Char('@'), 0, 0).toLongLong();
                    init.rangeStart = range.section(QLatin1Char('@'), 1, 1).toLongLong();
                }
                media->segments.append(init);
            }
            media->fragmentedMp4 = true;
        } else if (!line.startsWith(QLatin1Char('#'))) {
            MediaSegment segment;
            segment.url = resolveUrl(base, line);
            segment.duration = duration >= 0.0 ? duration : 0.0;
            if (rangeLength >= 0) {
                segment.rangeLength = rangeLength;
                segment.rangeStart = rangeOffset >= 0 ? rangeOffset : nextOffset;
                nextOffset = segment.rangeStart + rangeLength;
            }
            media->segments.append(segment);
            duration = -1.0;
            rangeLength = -1;
            rangeOffset = -1;
            if (media->segments.size() > kMaxStreamSegments) {
                *error = QStringLiteral("The playlist has too many pieces.");
                return false;
            }
        }
    }

    if (media->segments.isEmpty()) {
        *error = QStringLiteral("The playlist doesn't list any video.");
        return false;
    }

    if (media->fragmentedMp4) {
        media->extension = QStringLiteral("mp4");
    } else {
        const QString suffix = QFileInfo(QUrl(media->segments.first().url).path()).suffix().toLower();
        media->extension = (suffix == QLatin1String("aac") || suffix == QLatin1String("mp3")
                            || suffix == QLatin1String("mp4") || suffix == QLatin1String("m4a")
                            || suffix == QLatin1String("webm"))
            ? suffix
            : QStringLiteral("ts");
    }
    return true;
}

} // namespace haul
