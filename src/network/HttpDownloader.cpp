#include "HttpDownloader.h"

#include <QElapsedTimer>
#include <QFile>
#include <QThread>

namespace haul {

namespace {

struct SingleContext {
    QFile *file = nullptr;
    ResponseHeaders *headers = nullptr;
    const std::atomic_bool *cancel = nullptr;
    RateLimiter *limiter = nullptr;
    const ProgressCallback *onProgress = nullptr;
    QElapsedTimer reportTimer;

    qint64 resumeFrom = 0;
    qint64 expectedTotal = -1;
    qint64 total = -1;
    bool decided = false;
    bool restarted = false;
    bool discardPartial = false;
    TransferError failure;
};

// Called once, when the first body bytes of the final response arrive.
bool decideResponse(SingleContext &ctx)
{
    ctx.decided = true;
    const ResponseHeaders &headers = *ctx.headers;

    if (ctx.resumeFrom > 0 && headers.status == 416) {
        ctx.failure = { ErrorKind::Transient,
                        QStringLiteral("The server rejected the resume position; the download will restart."), 416, -1 };
        ctx.discardPartial = true;
        return false;
    }

    const ErrorKind statusKind = classifyHttpStatus(headers.status);
    if (statusKind != ErrorKind::None) {
        ctx.failure = { statusKind, describeHttpStatus(headers.status), headers.status,
                        parseRetryAfter(headers.value(QStringLiteral("retry-after"))) };
        return false;
    }

    if (ctx.resumeFrom > 0 && headers.status == 206) {
        const ContentRange range = parseContentRange(headers.value(QStringLiteral("content-range")));
        const bool totalMatches = ctx.expectedTotal < 0 || range.total < 0 || range.total == ctx.expectedTotal;
        if (!range.valid || range.start != ctx.resumeFrom || !totalMatches) {
            ctx.failure = { ErrorKind::Transient,
                            QStringLiteral("The server returned an unexpected byte range; the download will restart from the beginning."),
                            206, -1 };
            ctx.discardPartial = true;
            return false;
        }
        ctx.total = range.total >= 0 ? range.total : ctx.expectedTotal;
        return true;
    }

    if (headers.status == 206) {
        // We did not ask for a range, so a partial response cannot be trusted.
        ctx.failure = { ErrorKind::Transient, QStringLiteral("The server sent a partial response that was not requested."), 206, -1 };
        ctx.discardPartial = true;
        return false;
    }

    // 200 OK: the whole file is coming. If we had partial data it is now useless.
    if (ctx.resumeFrom > 0) {
        if (!ctx.file->resize(0) || !ctx.file->seek(0)) {
            ctx.failure = { ErrorKind::Permanent, QStringLiteral("Could not reset the partial file: %1").arg(ctx.file->errorString()), 0, -1 };
            return false;
        }
        ctx.resumeFrom = 0;
        ctx.restarted = true;
    }
    ctx.total = headers.contentLength();
    return true;
}

size_t singleWrite(char *data, size_t size, size_t count, void *userData)
{
    auto &ctx = *static_cast<SingleContext *>(userData);
    const size_t bytes = size * count;

    if (!ctx.decided && !decideResponse(ctx))
        return 0;

    const qint64 written = ctx.file->pos();
    if (ctx.total >= 0 && written + static_cast<qint64>(bytes) > ctx.total) {
        ctx.failure = { ErrorKind::Transient, QStringLiteral("The server sent more data than it announced."), 0, -1 };
        ctx.discardPartial = true;
        return 0;
    }

    if (ctx.file->write(data, static_cast<qint64>(bytes)) != static_cast<qint64>(bytes)) {
        ctx.failure = { ErrorKind::Permanent,
                        QStringLiteral("Could not write to disk (is it full or read-only?): %1").arg(ctx.file->errorString()),
                        0, -1 };
        return 0;
    }

    if (ctx.limiter)
        sleepUnlessCancelled(ctx.limiter->consume(static_cast<qint64>(bytes)), *ctx.cancel);
    return bytes;
}

int singleProgress(void *userData, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
{
    auto &ctx = *static_cast<SingleContext *>(userData);
    if (ctx.cancel->load())
        return 1;
    if (ctx.decided && ctx.onProgress && *ctx.onProgress && ctx.reportTimer.elapsed() >= 200) {
        ctx.reportTimer.restart();
        (*ctx.onProgress)(ctx.file->pos(), ctx.total);
    }
    return 0;
}

} // namespace

void sleepUnlessCancelled(qint64 ms, const std::atomic_bool &cancel)
{
    while (ms > 0 && !cancel.load()) {
        const qint64 step = ms < 50 ? ms : 50;
        QThread::msleep(static_cast<unsigned long>(step));
        ms -= step;
    }
}

TransferResult downloadSingle(const SingleRequest &request, const std::atomic_bool &cancel,
                              const ProgressCallback &onProgress)
{
    TransferResult result;
    result.totalBytes = request.expectedTotal;

    QFile file(request.filePath);
    const bool canResume = request.resume && file.exists() && file.size() > 0;
    const QIODevice::OpenMode mode = canResume ? (QIODevice::ReadWrite)
                                               : (QIODevice::WriteOnly | QIODevice::Truncate);
    if (!file.open(mode)) {
        result.error = { ErrorKind::Permanent,
                         QStringLiteral("Cannot write to the destination folder: %1").arg(file.errorString()), 0, -1 };
        return result;
    }

    // The bytes on disk are the source of truth for where to resume.
    qint64 resumeFrom = canResume ? file.size() : 0;
    if (request.expectedTotal >= 0 && resumeFrom > request.expectedTotal) {
        file.resize(0);
        resumeFrom = 0;
    }
    if (request.expectedTotal >= 0 && resumeFrom == request.expectedTotal && resumeFrom > 0) {
        // Already complete from a previous attempt.
        result.fileSize = resumeFrom;
        result.totalBytes = request.expectedTotal;
        return result;
    }
    file.seek(resumeFrom);

    CurlEasy curl;
    if (!curl.isValid()) {
        result.error = { ErrorKind::Permanent, QStringLiteral("Could not initialise libcurl."), 0, -1 };
        return result;
    }

    SingleContext ctx;
    ctx.file = &file;
    ctx.headers = &curl.response();
    ctx.cancel = &cancel;
    ctx.limiter = request.limiter;
    ctx.onProgress = &onProgress;
    ctx.resumeFrom = resumeFrom;
    ctx.expectedTotal = request.expectedTotal;
    ctx.reportTimer.start();

    curl.configure(request.url, request.options);
    if (resumeFrom > 0) {
        curl.addHeader("Range: bytes=" + QByteArray::number(resumeFrom) + "-");
        // Strong ETags are the most reliable validator; weak ones are not allowed in If-Range.
        if (!request.etag.isEmpty() && !request.etag.startsWith(QLatin1String("W/")))
            curl.addHeader("If-Range: " + request.etag.toLatin1());
        else if (!request.lastModified.isEmpty())
            curl.addHeader("If-Range: " + request.lastModified.toLatin1());
    }
    curl.applyHeaders();
    curl_easy_setopt(curl.handle(), CURLOPT_WRITEFUNCTION, &singleWrite);
    curl_easy_setopt(curl.handle(), CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(curl.handle(), CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl.handle(), CURLOPT_XFERINFOFUNCTION, &singleProgress);
    curl_easy_setopt(curl.handle(), CURLOPT_XFERINFODATA, &ctx);

    const CURLcode code = curl_easy_perform(curl.handle());

    if (!ctx.decided && code == CURLE_OK)
        decideResponse(ctx); // empty body: still validate the status code

    file.flush();
    result.fileSize = file.size();
    result.totalBytes = ctx.total;
    result.restartedFromZero = ctx.restarted;
    result.discardPartial = ctx.discardPartial;

    if (ctx.failure.isError()) {
        result.error = ctx.failure;
    } else if (code != CURLE_OK) {
        result.error = errorFromCurl(code, curl.errorBuffer());
    } else if (ctx.total >= 0 && result.fileSize != ctx.total) {
        result.error = { ErrorKind::Transient,
                         QStringLiteral("The connection closed before the whole file arrived (%1 of %2 bytes).")
                             .arg(result.fileSize).arg(ctx.total),
                         0, -1 };
    }
    file.close();

    if (onProgress && !result.error.isError())
        onProgress(result.fileSize, result.totalBytes);
    return result;
}

} // namespace haul
