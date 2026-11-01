#include "HttpProbe.h"

#include "core/FileNameUtils.h"

namespace haul {

namespace {

constexpr size_t kMaxProbeBody = 64 * 1024;

struct ProbeContext {
    size_t received = 0;
    bool stoppedOnPurpose = false;
    const std::atomic_bool *cancel = nullptr;
};

size_t probeWrite(char *, size_t size, size_t count, void *userData)
{
    auto *context = static_cast<ProbeContext *>(userData);
    context->received += size * count;
    if (context->received > kMaxProbeBody) {
        // The server ignored our range and is sending the whole file: we have seen enough.
        context->stoppedOnPurpose = true;
        return 0;
    }
    return size * count;
}

int probeProgress(void *userData, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
{
    auto *context = static_cast<ProbeContext *>(userData);
    return (context->cancel && context->cancel->load()) ? 1 : 0;
}

} // namespace

// FTP has no headers, so ask the server for the size and date without fetching the file.
static ProbeResult probeFtp(const QUrl &url, const NetworkOptions &options, const std::atomic_bool *cancel)
{
    ProbeResult result;
    CurlEasy curl;
    if (!curl.isValid()) {
        result.error = { ErrorKind::Permanent, QStringLiteral("Could not initialise libcurl."), 0, -1 };
        return result;
    }
    ProbeContext context;
    context.cancel = cancel;
    curl.configure(url, options);
    curl_easy_setopt(curl.handle(), CURLOPT_NOBODY, 1L);
    curl_easy_setopt(curl.handle(), CURLOPT_FILETIME, 1L);
    curl_easy_setopt(curl.handle(), CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl.handle(), CURLOPT_XFERINFOFUNCTION, &probeProgress);
    curl_easy_setopt(curl.handle(), CURLOPT_XFERINFODATA, &context);
    curl_easy_setopt(curl.handle(), CURLOPT_TIMEOUT, 60L);

    const CURLcode code = curl_easy_perform(curl.handle());
    if (code != CURLE_OK) {
        result.error = errorFromCurl(code, curl.errorBuffer());
        return result;
    }
    curl_off_t size = -1;
    curl_off_t modified = -1;
    curl_easy_getinfo(curl.handle(), CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &size);
    curl_easy_getinfo(curl.handle(), CURLINFO_FILETIME_T, &modified);
    result.totalSize = size >= 0 ? static_cast<qint64>(size) : -1;
    result.rangeSupported = true; // REST is near universal; a refusal is handled when resuming
    if (modified >= 0)
        result.lastModified = QString::number(static_cast<qint64>(modified));
    result.effectiveUrl = url.toString();
    result.suggestedName = fileNameFromUrl(url);
    result.ok = true;
    return result;
}

ProbeResult probeUrl(const QUrl &url, const NetworkOptions &options, const std::atomic_bool *cancel)
{
    if (isFtpUrl(url))
        return probeFtp(url, options, cancel);

    ProbeResult result;
    CurlEasy curl;
    if (!curl.isValid()) {
        result.error = { ErrorKind::Permanent, QStringLiteral("Could not initialise libcurl."), 0, -1 };
        return result;
    }

    ProbeContext context;
    context.cancel = cancel;

    curl.configure(url, options);
    curl.addHeader("Range: bytes=0-0");
    curl.applyHeaders();
    curl_easy_setopt(curl.handle(), CURLOPT_WRITEFUNCTION, &probeWrite);
    curl_easy_setopt(curl.handle(), CURLOPT_WRITEDATA, &context);
    curl_easy_setopt(curl.handle(), CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl.handle(), CURLOPT_XFERINFOFUNCTION, &probeProgress);
    curl_easy_setopt(curl.handle(), CURLOPT_XFERINFODATA, &context);
    curl_easy_setopt(curl.handle(), CURLOPT_TIMEOUT, 60L);

    const CURLcode code = curl_easy_perform(curl.handle());
    if (code != CURLE_OK && !(code == CURLE_WRITE_ERROR && context.stoppedOnPurpose)) {
        result.error = errorFromCurl(code, curl.errorBuffer());
        return result;
    }

    const ResponseHeaders &headers = curl.response();
    result.httpStatus = headers.status;
    result.effectiveUrl = curl.effectiveUrl();

    const ErrorKind statusKind = classifyHttpStatus(headers.status);
    if (statusKind != ErrorKind::None) {
        result.error.kind = statusKind;
        result.error.httpStatus = headers.status;
        result.error.message = describeHttpStatus(headers.status);
        result.error.retryAfterSeconds = parseRetryAfter(headers.value(QStringLiteral("retry-after")));
        return result;
    }

    if (headers.status == 206) {
        const ContentRange range = parseContentRange(headers.value(QStringLiteral("content-range")));
        if (range.valid && range.start == 0 && range.total > 0) {
            result.totalSize = range.total;
            result.rangeSupported = true;
        }
    } else {
        result.totalSize = headers.contentLength();
    }

    result.etag = headers.value(QStringLiteral("etag"));
    result.lastModified = headers.value(QStringLiteral("last-modified"));
    result.contentType = headers.value(QStringLiteral("content-type")).section(QLatin1Char(';'), 0, 0).trimmed();

    QString name = fileNameFromContentDisposition(headers.value(QStringLiteral("content-disposition")));
    if (name.isEmpty())
        name = fileNameFromUrl(QUrl(result.effectiveUrl.isEmpty() ? url.toString() : result.effectiveUrl));
    result.suggestedName = name;
    result.ok = true;
    return result;
}

} // namespace haul
