#pragma once

#include "core/RetryPolicy.h"

#include <QByteArray>
#include <QMap>
#include <QString>
#include <QUrl>

#include <curl/curl.h>

namespace haul {

struct NetworkOptions {
    int connectTimeoutSeconds = 20;
    int stallTimeoutSeconds = 60; // abort if fewer than 1 byte/s arrives for this long
    QByteArray userAgent = "Haul/0.1 (+https://github.com/tehsaad/Haul)";
};

// Headers of the final response. Redirect responses are discarded as they arrive.
struct ResponseHeaders {
    int status = 0;
    QMap<QString, QString> fields; // lower-case names

    void clear();
    QString value(const QString &name) const { return fields.value(name.toLower()); }
    qint64 contentLength() const; // -1 if absent or the body is encoded
};

// Parsed "Content-Range: bytes start-end/total" header.
struct ContentRange {
    bool valid = false;
    qint64 start = -1;
    qint64 end = -1;
    qint64 total = -1; // -1 for "*"
};

ContentRange parseContentRange(const QString &value);

// Retry-After as seconds (delta-seconds or HTTP-date). -1 if absent or invalid.
int parseRetryAfter(const QString &value);

// Builds a TransferError from a libcurl result code.
TransferError errorFromCurl(CURLcode code, const char *errorBuffer);

// RAII owner of one libcurl easy handle with Haul's safety defaults applied:
// only http/https (also for redirects), TLS verification on, bounded redirects and timeouts.
class CurlEasy
{
public:
    CurlEasy();
    ~CurlEasy();
    CurlEasy(const CurlEasy &) = delete;
    CurlEasy &operator=(const CurlEasy &) = delete;

    bool isValid() const { return m_curl != nullptr; }
    CURL *handle() const { return m_curl; }

    void configure(const QUrl &url, const NetworkOptions &options);
    void addHeader(const QByteArray &line);
    void applyHeaders();

    ResponseHeaders &response() { return m_response; }
    const char *errorBuffer() const { return m_errorBuffer; }
    QString effectiveUrl() const;

private:
    static size_t headerCallback(char *buffer, size_t size, size_t count, void *userData);

    CURL *m_curl = nullptr;
    curl_slist *m_headers = nullptr;
    ResponseHeaders m_response;
    char m_errorBuffer[CURL_ERROR_SIZE] = {};
};

} // namespace haul
