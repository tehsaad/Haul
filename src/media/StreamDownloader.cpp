#include "StreamDownloader.h"

#include "network/HttpDownloader.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>

#include <algorithm>
#include <deque>
#include <memory>
#include <vector>

namespace haul {

namespace {

constexpr qint64 kMaxInitSizeToScan = 4 * 1024 * 1024;

struct Piece {
    int track = 0;
    int index = 0;
    int attempts = 0;
    qint64 notBeforeMs = 0;
};

struct Connection {
    std::unique_ptr<CurlEasy> curl;
    std::unique_ptr<QFile> file;
    Piece piece;
    const MediaSegment *segment = nullptr;
    bool decided = false;
    qint64 written = 0;
    TransferError failure;
    RateLimiter *limiter = nullptr;
    const std::atomic_bool *cancel = nullptr;
};

TransferError refusedError(int status)
{
    if (status == 401 || status == 403 || status == 410) {
        return { ErrorKind::Permanent,
                 QStringLiteral("The server refused part of this video (HTTP %1). The link may have expired or need a login; open the page and try again.").arg(status),
                 status, -1 };
    }
    return { classifyHttpStatus(status), describeHttpStatus(status), status, -1 };
}

bool decide(Connection &c)
{
    c.decided = true;
    const ResponseHeaders &headers = c.curl->response();
    if (classifyHttpStatus(headers.status) != ErrorKind::None) {
        c.failure = refusedError(headers.status);
        return false;
    }
    if (headers.value(QStringLiteral("content-type")).startsWith(QLatin1String("text/html"), Qt::CaseInsensitive)) {
        c.failure = { ErrorKind::Permanent,
                      QStringLiteral("The server sent a web page instead of video. The link may have expired or need a login."), 0, -1 };
        return false;
    }
    if (c.segment->rangeStart >= 0) {
        const ContentRange range = parseContentRange(headers.value(QStringLiteral("content-range")));
        if (headers.status != 206 || !range.valid || range.start != c.segment->rangeStart) {
            c.failure = { ErrorKind::Transient, QStringLiteral("The server returned the wrong part of the video."), headers.status, -1 };
            return false;
        }
    }
    return true;
}

size_t pieceWrite(char *data, size_t size, size_t count, void *userData)
{
    auto &c = *static_cast<Connection *>(userData);
    const qint64 bytes = static_cast<qint64>(size * count);
    if (!c.decided && !decide(c))
        return 0;
    if (c.file->write(data, bytes) != bytes) {
        c.failure = { ErrorKind::Permanent, QStringLiteral("Could not write to disk (is it full or read-only?): %1").arg(c.file->errorString()), 0, -1 };
        return 0;
    }
    c.written += bytes;
    if (c.limiter)
        sleepUnlessCancelled(c.limiter->consume(bytes), *c.cancel);
    return static_cast<size_t>(bytes);
}

// A protected stream can leave the marker out of its manifest but not out of its init data.
bool initDeclaresEncryption(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    const QByteArray data = file.read(kMaxInitSizeToScan);
    return data.contains("encv") || data.contains("enca") || data.contains("sinf") || data.contains("pssh");
}

} // namespace

QString streamPiecePath(const QString &directory, int track, int index)
{
    return QStringLiteral("%1/t%2_%3.seg").arg(directory).arg(track).arg(index, 6, 10, QLatin1Char('0'));
}

qint64 streamBytesOnDisk(const QString &directory)
{
    qint64 total = 0;
    const QFileInfoList files = QDir(directory).entryInfoList({ QStringLiteral("*.seg") }, QDir::Files);
    for (const QFileInfo &file : files)
        total += file.size();
    return total;
}

StreamDownloadResult downloadStreamPieces(const StreamDownloadRequest &request, const std::atomic_bool &cancel,
                                          const StreamProgressCallback &onProgress)
{
    StreamDownloadResult result;
    if (!QDir().mkpath(request.directory)) {
        result.error = { ErrorKind::Permanent, QStringLiteral("Cannot write to the destination folder."), 0, -1 };
        return result;
    }

    std::deque<Piece> pending;
    for (int t = 0; t < request.tracks.size(); ++t) {
        const QVector<MediaSegment> &segments = request.tracks[t].segments;
        result.piecesTotal += segments.size();
        for (int i = 0; i < segments.size(); ++i) {
            const QFileInfo done(streamPiecePath(request.directory, t, i));
            if (done.exists()) {
                result.bytes += done.size();
                ++result.piecesDone;
            } else {
                pending.push_back({ t, i, 0, 0 });
            }
        }
    }

    CURLM *multi = curl_multi_init();
    if (!multi) {
        result.error = { ErrorKind::Permanent, QStringLiteral("Could not initialise libcurl."), 0, -1 };
        return result;
    }

    QElapsedTimer clock;
    clock.start();
    qint64 lastReport = -1000;
    std::vector<std::unique_ptr<Connection>> active;
    const int maxActive = std::clamp(request.connections, 1, 16);

    auto finishConnection = [&](Connection *c) {
        curl_multi_remove_handle(multi, c->curl->handle());
        active.erase(std::find_if(active.begin(), active.end(), [c](const auto &p) { return p.get() == c; }));
    };

    while (!result.error.isError()) {
        if (cancel.load()) {
            result.error = { ErrorKind::Cancelled, QStringLiteral("Stopped by the user."), 0, -1 };
            break;
        }

        // Start pieces up to the limit, always in playback order.
        while (static_cast<int>(active.size()) < maxActive && !pending.empty() && pending.front().notBeforeMs <= clock.elapsed()) {
            const Piece piece = pending.front();
            pending.pop_front();
            const MediaSegment &segment = request.tracks[piece.track].segments[piece.index];

            auto c = std::make_unique<Connection>();
            c->piece = piece;
            c->segment = &segment;
            c->limiter = request.limiter;
            c->cancel = &cancel;
            c->file = std::make_unique<QFile>(streamPiecePath(request.directory, piece.track, piece.index) + QStringLiteral(".part"));
            if (!c->file->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                result.error = { ErrorKind::Permanent, QStringLiteral("Cannot write to the destination folder: %1").arg(c->file->errorString()), 0, -1 };
                break;
            }
            c->curl = std::make_unique<CurlEasy>();
            c->curl->configure(QUrl(segment.url), request.options);
            if (segment.rangeStart >= 0) {
                c->curl->addHeader("Range: bytes=" + QByteArray::number(segment.rangeStart) + "-"
                                   + QByteArray::number(segment.rangeStart + segment.rangeLength - 1));
            }
            c->curl->applyHeaders();
            curl_easy_setopt(c->curl->handle(), CURLOPT_WRITEFUNCTION, &pieceWrite);
            curl_easy_setopt(c->curl->handle(), CURLOPT_WRITEDATA, c.get());
            curl_easy_setopt(c->curl->handle(), CURLOPT_PRIVATE, c.get());
            curl_multi_add_handle(multi, c->curl->handle());
            active.push_back(std::move(c));
        }
        if (result.error.isError())
            break;

        if (active.empty() && pending.empty())
            break; // everything is done

        int running = 0;
        const CURLMcode mc = curl_multi_perform(multi, &running);
        if (mc != CURLM_OK) {
            result.error = { ErrorKind::Transient, QString::fromUtf8(curl_multi_strerror(mc)), 0, -1 };
            break;
        }

        int queued = 0;
        while (CURLMsg *message = curl_multi_info_read(multi, &queued)) {
            if (message->msg != CURLMSG_DONE)
                continue;
            Connection *c = nullptr;
            curl_easy_getinfo(message->easy_handle, CURLINFO_PRIVATE, &c);
            const CURLcode code = message->data.result;

            if (!c->decided && code == CURLE_OK)
                decide(*c); // empty body: the status still has to be checked

            TransferError failure = c->failure;
            if (!failure.isError() && code != CURLE_OK)
                failure = errorFromCurl(code, c->curl->errorBuffer());
            if (!failure.isError() && c->segment->rangeLength >= 0 && c->written != c->segment->rangeLength)
                failure = { ErrorKind::Transient, QStringLiteral("A piece of the video ended early."), 0, -1 };
            if (!failure.isError()) {
                const qint64 expected = c->curl->response().contentLength();
                if (expected >= 0 && c->written != expected)
                    failure = { ErrorKind::Transient, QStringLiteral("A piece of the video ended early."), 0, -1 };
            }

            c->file->flush();
            const QString partPath = c->file->fileName();
            c->file->close();

            if (!failure.isError()) {
                const QString finalPath = streamPiecePath(request.directory, c->piece.track, c->piece.index);
                if (c->segment->isInit && initDeclaresEncryption(partPath)) {
                    QFile::remove(partPath);
                    result.error = { ErrorKind::Permanent, QString::fromLatin1(kEncryptedStreamMessage), 0, -1 };
                } else if (!QFile::rename(partPath, finalPath)) {
                    result.error = { ErrorKind::Permanent, QStringLiteral("Could not save part of the video."), 0, -1 };
                } else {
                    result.bytes += c->written;
                    ++result.piecesDone;
                }
            } else {
                QFile::remove(partPath);
                Piece retry = c->piece;
                ++retry.attempts;
                if (failure.kind == ErrorKind::Transient && retry.attempts < request.attemptsPerPiece) {
                    retry.notBeforeMs = clock.elapsed() + request.retryDelayMs * retry.attempts;
                    pending.push_back(retry);
                    std::stable_sort(pending.begin(), pending.end(), [](const Piece &a, const Piece &b) {
                        return a.track != b.track ? a.track < b.track : a.index < b.index;
                    });
                } else {
                    result.error = failure;
                }
            }
            finishConnection(c);
            if (result.error.isError())
                break;
        }

        if (onProgress && clock.elapsed() - lastReport >= 200) {
            lastReport = clock.elapsed();
            qint64 inFlight = 0;
            for (const auto &c : active)
                inFlight += c->written;
            onProgress(result.bytes + inFlight, result.piecesDone, result.piecesTotal);
        }

        if (!active.empty())
            curl_multi_poll(multi, nullptr, 0, 100, nullptr);
        else if (!pending.empty())
            sleepUnlessCancelled(50, cancel);
    }

    for (const auto &c : active) {
        curl_multi_remove_handle(multi, c->curl->handle());
        const QString partPath = c->file->fileName();
        c->file->close();
        QFile::remove(partPath);
    }
    active.clear();
    curl_multi_cleanup(multi);

    if (onProgress)
        onProgress(result.bytes, result.piecesDone, result.piecesTotal);
    return result;
}

} // namespace haul
