#include "SegmentDownloader.h"

#include "HttpDownloader.h"

#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>

#include <memory>
#include <vector>

namespace haul {

namespace {

struct SegmentConnection {
    std::unique_ptr<CurlEasy> curl;
    std::unique_ptr<QFile> file;
    SegmentInfo *segment = nullptr;
    qint64 requestStart = 0;
    qint64 totalBytes = 0;
    bool decided = false;
    bool rangeIgnored = false;
    TransferError failure;
    RateLimiter *limiter = nullptr;
    const std::atomic_bool *cancel = nullptr;
};

bool decideSegment(SegmentConnection &c)
{
    c.decided = true;
    const ResponseHeaders &headers = c.curl->response();

    if (headers.status == 200) {
        c.rangeIgnored = true;
        return false;
    }
    const ErrorKind statusKind = classifyHttpStatus(headers.status);
    if (statusKind != ErrorKind::None) {
        c.failure = { statusKind, describeHttpStatus(headers.status), headers.status,
                      parseRetryAfter(headers.value(QStringLiteral("retry-after"))) };
        return false;
    }
    if (headers.status != 206) {
        c.failure = { ErrorKind::Transient, QStringLiteral("Unexpected HTTP %1 for a segment.").arg(headers.status), headers.status, -1 };
        return false;
    }

    const ContentRange range = parseContentRange(headers.value(QStringLiteral("content-range")));
    if (!range.valid || range.start != c.requestStart || range.end != c.segment->end
        || (range.total >= 0 && range.total != c.totalBytes)) {
        c.failure = { ErrorKind::Transient,
                      QStringLiteral("Segment %1: the server returned a different byte range than requested.")
                          .arg(c.segment->index + 1),
                      206, -1 };
        return false;
    }
    return true;
}

size_t segmentWrite(char *data, size_t size, size_t count, void *userData)
{
    auto &c = *static_cast<SegmentConnection *>(userData);
    const qint64 bytes = static_cast<qint64>(size * count);

    if (!c.decided && !decideSegment(c))
        return 0;

    if (c.segment->downloaded + bytes > c.segment->length()) {
        c.failure = { ErrorKind::Transient,
                      QStringLiteral("Segment %1: the server sent more data than requested.").arg(c.segment->index + 1), 0, -1 };
        return 0;
    }
    if (c.file->write(data, bytes) != bytes) {
        c.failure = { ErrorKind::Permanent,
                      QStringLiteral("Could not write to disk (is it full or read-only?): %1").arg(c.file->errorString()), 0, -1 };
        return 0;
    }
    c.segment->downloaded += bytes;

    if (c.limiter)
        sleepUnlessCancelled(c.limiter->consume(bytes), *c.cancel);
    return static_cast<size_t>(bytes);
}

} // namespace

SegmentedResult downloadSegments(const SegmentedRequest &request, const std::atomic_bool &cancel,
                                 const SegmentProgressCallback &onProgress)
{
    SegmentedResult result;
    result.segments = request.segments;

    CURLM *multi = curl_multi_init();
    if (!multi) {
        result.error = { ErrorKind::Permanent, QStringLiteral("Could not initialise libcurl."), 0, -1 };
        return result;
    }

    std::vector<std::unique_ptr<SegmentConnection>> connections;

    for (SegmentInfo &segment : result.segments) {
        auto file = std::make_unique<QFile>(request.segmentPath(segment.index));
        const bool exists = file->exists();
        if (!file->open(exists ? QIODevice::ReadWrite : QIODevice::WriteOnly)) {
            result.error = { ErrorKind::Permanent,
                             QStringLiteral("Cannot write to the destination folder: %1").arg(file->errorString()), 0, -1 };
            break;
        }
        // Resume each segment from the bytes already on disk (never trust more than its length).
        if (file->size() > segment.length())
            file->resize(segment.length());
        segment.downloaded = file->size();
        file->seek(segment.downloaded);
        if (segment.isComplete())
            continue;

        auto c = std::make_unique<SegmentConnection>();
        c->curl = std::make_unique<CurlEasy>();
        c->file = std::move(file);
        c->segment = &segment;
        c->requestStart = segment.start + segment.downloaded;
        c->totalBytes = request.totalBytes;
        c->limiter = request.limiter;
        c->cancel = &cancel;

        CurlEasy &curl = *c->curl;
        curl.configure(request.url, request.options);
        curl.addHeader("Range: bytes=" + QByteArray::number(c->requestStart) + "-" + QByteArray::number(segment.end));
        if (!request.etag.isEmpty() && !request.etag.startsWith(QLatin1String("W/")))
            curl.addHeader("If-Range: " + request.etag.toLatin1());
        else if (!request.lastModified.isEmpty())
            curl.addHeader("If-Range: " + request.lastModified.toLatin1());
        curl.applyHeaders();
        curl_easy_setopt(curl.handle(), CURLOPT_WRITEFUNCTION, &segmentWrite);
        curl_easy_setopt(curl.handle(), CURLOPT_WRITEDATA, c.get());
        curl_easy_setopt(curl.handle(), CURLOPT_PRIVATE, c.get());
        curl_multi_add_handle(multi, curl.handle());
        connections.push_back(std::move(c));
    }

    QElapsedTimer reportTimer;
    reportTimer.start();
    int running = 0;

    while (!result.error.isError()) {
        if (cancel.load()) {
            result.error = { ErrorKind::Cancelled, QStringLiteral("Stopped by the user."), 0, -1 };
            break;
        }
        const CURLMcode mc = curl_multi_perform(multi, &running);
        if (mc != CURLM_OK) {
            result.error = { ErrorKind::Transient, QString::fromUtf8(curl_multi_strerror(mc)), 0, -1 };
            break;
        }

        int queued = 0;
        while (CURLMsg *message = curl_multi_info_read(multi, &queued)) {
            if (message->msg != CURLMSG_DONE)
                continue;
            SegmentConnection *c = nullptr;
            curl_easy_getinfo(message->easy_handle, CURLINFO_PRIVATE, &c);
            const CURLcode code = message->data.result;

            if (c->rangeIgnored) {
                result.rangeUnsupported = true;
                result.error = { ErrorKind::Transient, QStringLiteral("The server ignored a byte-range request."), 200, -1 };
            } else if (c->failure.isError()) {
                result.error = c->failure;
            } else if (code != CURLE_OK) {
                result.error = errorFromCurl(code, c->curl->errorBuffer());
            } else if (!c->segment->isComplete()) {
                result.error = { ErrorKind::Transient,
                                 QStringLiteral("Segment %1 ended early.").arg(c->segment->index + 1), 0, -1 };
            }
            if (result.error.isError())
                break;
        }

        if (onProgress && reportTimer.elapsed() >= 200) {
            reportTimer.restart();
            onProgress(result.segments);
        }
        if (running == 0 || result.error.isError())
            break;
        curl_multi_poll(multi, nullptr, 0, 100, nullptr);
    }

    for (auto &c : connections) {
        curl_multi_remove_handle(multi, c->curl->handle());
        c->file->flush();
        c->file->close();
    }
    curl_multi_cleanup(multi);

    if (onProgress)
        onProgress(result.segments);
    return result;
}

bool assembleSegments(const QVector<SegmentInfo> &segments, const SegmentPathFunction &segmentPath,
                      const QString &outputPath, qint64 totalBytes, const std::atomic_bool &cancel,
                      QString *error)
{
    // Verify every piece before touching the output.
    for (const SegmentInfo &segment : segments) {
        const qint64 size = QFileInfo(segmentPath(segment.index)).size();
        if (size != segment.length()) {
            *error = QStringLiteral("Segment %1 has %2 bytes but %3 were expected.")
                         .arg(segment.index + 1).arg(size).arg(segment.length());
            return false;
        }
    }

    QFile output(outputPath);
    if (!output.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        *error = QStringLiteral("Cannot create the output file: %1").arg(output.errorString());
        return false;
    }

    QByteArray buffer;
    for (const SegmentInfo &segment : segments) {
        QFile part(segmentPath(segment.index));
        if (!part.open(QIODevice::ReadOnly)) {
            *error = QStringLiteral("Cannot read segment %1: %2").arg(segment.index + 1).arg(part.errorString());
            return false;
        }
        while (!part.atEnd()) {
            if (cancel.load()) {
                *error = QStringLiteral("Stopped by the user.");
                return false;
            }
            buffer = part.read(1 << 20);
            if (output.write(buffer) != buffer.size()) {
                *error = QStringLiteral("Could not write to disk: %1").arg(output.errorString());
                return false;
            }
        }
    }
    output.flush();
    if (output.size() != totalBytes) {
        *error = QStringLiteral("The assembled file has %1 bytes but %2 were expected.").arg(output.size()).arg(totalBytes);
        return false;
    }
    output.close();

    for (const SegmentInfo &segment : segments)
        QFile::remove(segmentPath(segment.index));
    return true;
}

} // namespace haul
