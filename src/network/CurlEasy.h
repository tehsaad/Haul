#pragma once

#include "core/RetryPolicy.h"

#include <QByteArray>
#include <QMap>
#include <QString>
#include <QUrl>

#include <curl/curl.h>

namespace haul {

// Optional login for one download. Held in memory only: never written to the database,
// settings or logs, so it is forgotten when Haul closes.
struct DownloadLogin {
    QString username;
    QString password;
    QByteArray cookie; // "name=value; name2=value2", typed by the user
    QByteArray referer; // site the link came from; some servers refuse downloads without it

    bool isEmpty() const { return username.isEmpty() && password.isEmpty() && cookie.isEmpty() && referer.isEmpty(); }
};

inline bool isFtpUrl(const QUrl &url)
{
    const QString scheme = url.scheme().toLower();
    return scheme == QLatin1String("ftp") || scheme == QLatin1String("ftps");
}

struct NetworkOptions {
    int connectTimeoutSeconds = 20;
    int stallTimeoutSeconds = 60; // abort if fewer than 1 byte/s arrives for this long
    QByteArray userAgent = "Haul/0.3 (+https://github.com/tehsaad/Haul)";
    DownloadLogin login;
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
// only http, https and ftp (redirects may only go to http or https), TLS verification on, bounded redirects and timeouts.
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
