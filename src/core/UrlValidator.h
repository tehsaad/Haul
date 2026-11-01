#pragma once

#include <QString>
#include <QUrl>

namespace haul {

struct UrlCheck {
    bool ok = false;
    QString error; // user-facing explanation when !ok
    QUrl url;      // normalized URL when ok
};

// Accepts only absolute http, https, ftp and ftps URLs with a host.
// Rejects file://, javascript:, data:, embedded credentials and malformed input.
UrlCheck validateDownloadUrl(const QString &input);

// Returns the URL without user info, query string or fragment, so it is safe to log.
QString redactUrl(const QUrl &url);

} // namespace haul
