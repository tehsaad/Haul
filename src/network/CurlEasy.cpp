#include "CurlEasy.h"

#include <QDateTime>
#include <QLocale>
#include <QRegularExpression>
#include <QTimeZone>

namespace haul {

void ResponseHeaders::clear()
{
    status = 0;
    fields.clear();
}

qint64 ResponseHeaders::contentLength() const
{
    // A compressed body has a different length than the file we store, so ignore it.
    const QString encoding = value(QStringLiteral("content-encoding")).trimmed().toLower();
    if (!encoding.isEmpty() && encoding != QLatin1String("identity"))
        return -1;
    bool ok = false;
    const qint64 length = value(QStringLiteral("content-length")).trimmed().toLongLong(&ok);
    return ok && length >= 0 ? length : -1;
}

ContentRange parseContentRange(const QString &value)
{
    static const QRegularExpression pattern(
        QStringLiteral(R"(^\s*bytes\s+(\d+)-(\d+)/(\d+|\*)\s*$)"),
        QRegularExpression::CaseInsensitiveOption);

    ContentRange range;
    const QRegularExpressionMatch match = pattern.match(value);
    if (!match.hasMatch())
        return range;

    bool okStart = false;
    bool okEnd = false;
    range.start = match.captured(1).toLongLong(&okStart);
    range.end = match.captured(2).toLongLong(&okEnd);
    if (match.captured(3) == QLatin1String("*")) {
        range.total = -1;
    } else {
        bool okTotal = false;
        range.total = match.captured(3).toLongLong(&okTotal);
        if (!okTotal)
            return ContentRange();
    }
    range.valid = okStart && okEnd && range.start <= range.end
        && (range.total < 0 || range.end < range.total);
    return range;
}

int parseRetryAfter(const QString &value)
{
    const QString text = value.trimmed();
    if (text.isEmpty())
        return -1;
    bool ok = false;
    const int seconds = text.toInt(&ok);
    if (ok)
        return seconds >= 0 ? seconds : -1;

    const QDateTime when = QLocale::c().toDateTime(text, QStringLiteral("ddd, dd MMM yyyy hh:mm:ss 'GMT'"));
    if (!when.isValid())
        return -1;
    QDateTime utc = when;
    utc.setTimeZone(QTimeZone::utc());
    const qint64 delta = QDateTime::currentDateTimeUtc().secsTo(utc);
    return delta > 0 ? static_cast<int>(delta) : 0;
}

TransferError errorFromCurl(CURLcode code, const char *errorBuffer)
{
    TransferError error;
    if (code == CURLE_OK)
        return error;

    const QString detail = (errorBuffer && errorBuffer[0]) ? QString::fromUtf8(errorBuffer)
                                                         : QString::fromUtf8(curl_easy_strerror(code));
    error.message = detail;

    switch (code) {
    case CURLE_ABORTED_BY_CALLBACK:
        error.kind = ErrorKind::Cancelled;
        error.message = QStringLiteral("Stopped by the user.");
        break;
    case CURLE_COULDNT_RESOLVE_HOST:
        error.kind = ErrorKind::Transient;
        error.message = QStringLiteral("Could not find the server (DNS lookup failed): %1").arg(detail);
        break;
    case CURLE_COULDNT_RESOLVE_PROXY:
    case CURLE_COULDNT_CONNECT:
        error.kind = ErrorKind::Transient;
        error.message = QStringLiteral("Could not connect to the server: %1").arg(detail);
        break;
    case CURLE_OPERATION_TIMEDOUT:
        error.kind = ErrorKind::Transient;
        error.message = QStringLiteral("The connection timed out: %1").arg(detail);
        break;
    case CURLE_PARTIAL_FILE:
    case CURLE_RECV_ERROR:
    case CURLE_SEND_ERROR:
    case CURLE_GOT_NOTHING:
    case CURLE_HTTP2:
    case CURLE_HTTP2_STREAM:
    case CURLE_HTTP3:
    case CURLE_SSL_CONNECT_ERROR:
        error.kind = ErrorKind::Transient;
        error.message = QStringLiteral("The connection was interrupted: %1").arg(detail);
        break;
    case CURLE_PEER_FAILED_VERIFICATION:
    case CURLE_SSL_CACERT_BADFILE:
    case CURLE_SSL_CERTPROBLEM:
        error.kind = ErrorKind::Permanent;
        error.message = QStringLiteral("The server's TLS certificate could not be verified: %1").arg(detail);
        break;
    case CURLE_LOGIN_DENIED:
        error.kind = ErrorKind::Permanent;
        error.message = QStringLiteral("The server rejected the login. Right-click the download and choose Log in.");
        break;
    case CURLE_REMOTE_FILE_NOT_FOUND:
        error.kind = ErrorKind::Permanent;
        error.message = QStringLiteral("The file does not exist on the server.");
        break;
    case CURLE_REMOTE_ACCESS_DENIED:
        error.kind = ErrorKind::Permanent;
        error.message = QStringLiteral("The server refused access to this file.");
        break;
    case CURLE_UNSUPPORTED_PROTOCOL:
        error.kind = ErrorKind::Permanent;
        error.message = QStringLiteral("Unsupported protocol (only http, https and ftp are allowed).");
        break;
    case CURLE_URL_MALFORMAT:
        error.kind = ErrorKind::Permanent;
        error.message = QStringLiteral("The URL is malformed.");
        break;
    case CURLE_TOO_MANY_REDIRECTS:
        error.kind = ErrorKind::Permanent;
        error.message = QStringLiteral("The server redirected too many times.");
        break;
    case CURLE_WRITE_ERROR:
        error.kind = ErrorKind::Permanent;
        error.message = QStringLiteral("Could not write the downloaded data to disk.");
        break;
    default:
        error.kind = ErrorKind::Transient;
        break;
    }
    return error;
}

CurlEasy::CurlEasy()
    : m_curl(curl_easy_init())
{
}

CurlEasy::~CurlEasy()
{
    if (m_headers)
        curl_slist_free_all(m_headers);
    if (m_curl)
        curl_easy_cleanup(m_curl);
}

void CurlEasy::configure(const QUrl &url, const NetworkOptions &options)
{
    const QByteArray encodedUrl = url.toEncoded();
    curl_easy_setopt(m_curl, CURLOPT_URL, encodedUrl.constData());
    curl_easy_setopt(m_curl, CURLOPT_ERRORBUFFER, m_errorBuffer);

    // Security: never allow file:// and friends. A web server cannot redirect us to FTP either.
    curl_easy_setopt(m_curl, CURLOPT_PROTOCOLS_STR, "http,https,ftp,ftps");
    curl_easy_setopt(m_curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(m_curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(m_curl, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(m_curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(m_curl, CURLOPT_SSL_VERIFYHOST, 2L);

    curl_easy_setopt(m_curl, CURLOPT_CONNECTTIMEOUT, static_cast<long>(options.connectTimeoutSeconds));
    curl_easy_setopt(m_curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(m_curl, CURLOPT_LOW_SPEED_TIME, static_cast<long>(options.stallTimeoutSeconds));
    curl_easy_setopt(m_curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(m_curl, CURLOPT_USERAGENT, options.userAgent.constData());

    curl_easy_setopt(m_curl, CURLOPT_HEADERFUNCTION, &CurlEasy::headerCallback);
    curl_easy_setopt(m_curl, CURLOPT_HEADERDATA, &m_response);

    // libcurl does not resend these to a different host after a redirect.
    if (!options.login.username.isEmpty() || !options.login.password.isEmpty()) {
        const QByteArray user = options.login.username.toUtf8();
        const QByteArray password = options.login.password.toUtf8();
        curl_easy_setopt(m_curl, CURLOPT_USERNAME, user.constData());
        curl_easy_setopt(m_curl, CURLOPT_PASSWORD, password.constData());
        curl_easy_setopt(m_curl, CURLOPT_HTTPAUTH, CURLAUTH_ANY);
    }
    if (!options.login.cookie.isEmpty())
        curl_easy_setopt(m_curl, CURLOPT_COOKIE, options.login.cookie.constData());

    // We store exactly the bytes the server sends, so ask for an unencoded body.
    addHeader("Accept-Encoding: identity");
}

void CurlEasy::addHeader(const QByteArray &line)
{
    m_headers = curl_slist_append(m_headers, line.constData());
}

void CurlEasy::applyHeaders()
{
    curl_easy_setopt(m_curl, CURLOPT_HTTPHEADER, m_headers);
}

QString CurlEasy::effectiveUrl() const
{
    char *url = nullptr;
    if (curl_easy_getinfo(m_curl, CURLINFO_EFFECTIVE_URL, &url) == CURLE_OK && url)
        return QString::fromUtf8(url);
    return QString();
}

size_t CurlEasy::headerCallback(char *buffer, size_t size, size_t count, void *userData)
{
    auto *response = static_cast<ResponseHeaders *>(userData);
    const size_t total = size * count;
    const QString line = QString::fromLatin1(buffer, static_cast<int>(total)).trimmed();

    if (line.startsWith(QLatin1String("HTTP/"), Qt::CaseInsensitive)) {
        // A new response begins (for example after a redirect): forget the previous headers.
        response->clear();
        const QStringList parts = line.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (parts.size() >= 2)
            response->status = parts.at(1).toInt();
        return total;
    }

    const int colon = line.indexOf(QLatin1Char(':'));
    if (colon > 0 && response->fields.size() < 200) {
        const QString name = line.left(colon).trimmed().toLower();
        const QString value = line.mid(colon + 1).trimmed().left(4096);
        response->fields.insert(name, value);
    }
    return total;
}

} // namespace haul
