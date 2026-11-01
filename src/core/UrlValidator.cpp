#include "UrlValidator.h"

namespace haul {

namespace {
constexpr int kMaxUrlLength = 8192;
}

UrlCheck validateDownloadUrl(const QString &input)
{
    UrlCheck result;
    const QString text = input.trimmed();

    if (text.isEmpty()) {
        result.error = QStringLiteral("Enter a download URL.");
        return result;
    }
    if (text.size() > kMaxUrlLength) {
        result.error = QStringLiteral("The URL is too long.");
        return result;
    }
    for (const QChar c : text) {
        if (c.isSpace() || c.category() == QChar::Other_Control) {
            result.error = QStringLiteral("The URL contains spaces or control characters.");
            return result;
        }
    }

    const QUrl url(text, QUrl::StrictMode);
    if (!url.isValid()) {
        result.error = QStringLiteral("The URL is malformed.");
        return result;
    }

    const QString scheme = url.scheme().toLower();
    if (scheme.isEmpty()) {
        result.error = QStringLiteral("The URL must start with http://, https:// or ftp://.");
        return result;
    }
    if (scheme != QLatin1String("http") && scheme != QLatin1String("https")
        && scheme != QLatin1String("ftp") && scheme != QLatin1String("ftps")) {
        result.error = QStringLiteral("Only http, https and ftp links are supported (got \"%1:\").")
                           .arg(scheme);
        return result;
    }
    if (url.host().isEmpty()) {
        result.error = QStringLiteral("The URL has no host name.");
        return result;
    }
    if (!url.userInfo().isEmpty()) {
        result.error = QStringLiteral("Don't put a password in the link. Use the Login fields instead.");
        return result;
    }

    result.ok = true;
    result.url = url.adjusted(QUrl::RemoveFragment);
    return result;
}

QString redactUrl(const QUrl &url)
{
    QUrl copy = url.adjusted(QUrl::RemoveUserInfo | QUrl::RemoveFragment);
    const bool hadQuery = copy.hasQuery();
    copy.setQuery(QString());
    QString text = copy.toString(QUrl::RemoveQuery);
    if (hadQuery)
        text += QStringLiteral("?…");
    return text;
}

} // namespace haul
