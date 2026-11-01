#include "BrowserIntegration.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QStandardPaths>

namespace haul::platform {

const char *const kNativeHostName = "io.github.tehsaad.haul";
const char *const kChromeExtensionId = "flocbanlkaogdjjafbpkkijbpjipnnbm";
const char *const kFirefoxExtensionId = "haul@tehsaad.github.io";

bool isAllowedBrowserCaller(const QStringList &arguments)
{
    if (arguments.contains(QStringLiteral("chrome-extension://%1/").arg(QLatin1String(kChromeExtensionId))))
        return true;
    return arguments.contains(QString::fromLatin1(kFirefoxExtensionId));
}

bool parseBrowserMessage(const QByteArray &json, IpcCommand *command, QString *error)
{
    const QJsonObject o = QJsonDocument::fromJson(json).object();
    if (o.value("type").toString() != QLatin1String("add")) {
        if (error)
            *error = QStringLiteral("Unknown message.");
        return false;
    }
    QJsonObject forwarded;
    forwarded["cmd"] = QStringLiteral("add");
    forwarded["url"] = o.value("url").toString();
    forwarded["name"] = o.value("filename").toString();
    forwarded["referrer"] = o.value("referrer").toString();
    forwarded["stream"] = o.value("stream").toString();
    forwarded["variant"] = o.value("variant").toString();
    forwarded["confirm"] = true; // a browser can suggest a download, never start one by itself
    return decodeIpcCommand(QJsonDocument(forwarded).toJson(QJsonDocument::Compact), command, error);
}

bool isInspectMessage(const QByteArray &json, QUrl *url, QByteArray *referer)
{
    const QJsonObject o = QJsonDocument::fromJson(json).object();
    if (o.value("type").toString() != QLatin1String("inspect"))
        return false;
    *url = QUrl(o.value("url").toString());
    const QString origin = o.value("referrer").toString();
    if (origin.startsWith(QLatin1String("http")))
        *referer = origin.toUtf8();
    return true;
}

QJsonObject describeStream(const StreamInfo &info)
{
    QJsonObject o;
    o["ok"] = info.ok;
    o["kind"] = streamKindKey(info.kind);
    if (!info.ok) {
        o["error"] = info.error.message;
        o["encrypted"] = info.error.message == QLatin1String(kEncryptedStreamMessage);
        o["restricted"] = info.error.httpStatus == 401 || info.error.httpStatus == 403;
        return o;
    }
    o["duration"] = info.duration;
    QJsonArray variants;
    for (int i = 0; i < info.variants.size() && i < 30; ++i) {
        const StreamVariant &variant = info.variants[i];
        QJsonObject v;
        v["id"] = variant.id;
        v["label"] = variant.displayName;
        v["size"] = static_cast<double>(variant.estimatedBytes);
        variants.append(v);
    }
    o["variants"] = variants;
    QJsonArray children;
    for (int i = 0; i < info.childUrls.size() && i < 60; ++i)
        children.append(info.childUrls[i]);
    o["children"] = children;
    return o;
}

QByteArray nativeHostManifest(const QString &hostPath, bool firefox)
{
    QJsonObject o;
    o["name"] = QString::fromLatin1(kNativeHostName);
    o["description"] = QStringLiteral("Sends links from the browser to Haul");
    o["path"] = QDir::toNativeSeparators(hostPath);
    o["type"] = QStringLiteral("stdio");
    if (firefox)
        o["allowed_extensions"] = QJsonArray { QString::fromLatin1(kFirefoxExtensionId) };
    else
        o["allowed_origins"] = QJsonArray { QStringLiteral("chrome-extension://%1/").arg(QLatin1String(kChromeExtensionId)) };
    return QJsonDocument(o).toJson();
}

static QString hostExecutable()
{
#if defined(Q_OS_WIN)
    const char *name = "/haul-native-host.exe";
#else
    const char *name = "/haul-native-host";
#endif
    return QCoreApplication::applicationDirPath() + QLatin1String(name);
}

static bool writeFile(const QString &path, const QByteArray &data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(data) == data.size();
}

#if defined(Q_OS_WIN)

static const char *const kRegistryRoots[] = {
    "HKEY_CURRENT_USER\\Software\\Google\\Chrome\\NativeMessagingHosts\\",
    "HKEY_CURRENT_USER\\Software\\Microsoft\\Edge\\NativeMessagingHosts\\",
    "HKEY_CURRENT_USER\\Software\\BraveSoftware\\Brave-Browser\\NativeMessagingHosts\\",
    "HKEY_CURRENT_USER\\Software\\Mozilla\\NativeMessagingHosts\\",
};

static QString manifestFolder()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
}

bool isBrowserIntegrationEnabled()
{
    QSettings key(QString::fromLatin1(kRegistryRoots[0]) + QLatin1String(kNativeHostName), QSettings::NativeFormat);
    return key.contains(QStringLiteral("."));
}

bool setBrowserIntegrationEnabled(bool enabled, QString *error)
{
    const QString chromium = QDir(manifestFolder()).filePath(QStringLiteral("native-host-chromium.json"));
    const QString firefox = QDir(manifestFolder()).filePath(QStringLiteral("native-host-firefox.json"));
    if (enabled && (!QFile::exists(hostExecutable()) || !writeFile(chromium, nativeHostManifest(hostExecutable(), false))
                    || !writeFile(firefox, nativeHostManifest(hostExecutable(), true)))) {
        if (error)
            *error = QStringLiteral("Could not set up the browser helper.");
        return false;
    }
    for (const char *root : kRegistryRoots) {
        QSettings key(QString::fromLatin1(root) + QLatin1String(kNativeHostName), QSettings::NativeFormat);
        const bool isFirefox = QString::fromLatin1(root).contains(QLatin1String("Mozilla"));
        if (enabled)
            key.setValue(QStringLiteral("."), QDir::toNativeSeparators(isFirefox ? firefox : chromium));
        else
            key.remove(QString());
    }
    if (!enabled) {
        QFile::remove(chromium);
        QFile::remove(firefox);
    }
    return true;
}

#else

static QStringList manifestPaths(bool firefox)
{
    const QString home = QDir::homePath();
    const QString file = QLatin1String(kNativeHostName) + QStringLiteral(".json");
    QStringList folders;
    if (firefox) {
        folders << home + QStringLiteral("/.mozilla/native-messaging-hosts");
    } else {
        const QString config = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
        for (const char *browser : { "google-chrome", "chromium", "BraveSoftware/Brave-Browser", "microsoft-edge", "vivaldi" })
            folders << config + QLatin1Char('/') + QLatin1String(browser) + QStringLiteral("/NativeMessagingHosts");
    }
    QStringList paths;
    for (const QString &folder : std::as_const(folders))
        paths << QDir(folder).filePath(file);
    return paths;
}

bool isBrowserIntegrationEnabled()
{
    for (bool firefox : { false, true }) {
        for (const QString &path : manifestPaths(firefox)) {
            if (QFile::exists(path))
                return true;
        }
    }
    return false;
}

bool setBrowserIntegrationEnabled(bool enabled, QString *error)
{
    if (enabled && !QFile::exists(hostExecutable())) {
        if (error)
            *error = QStringLiteral("The browser helper (haul-native-host) is not installed.");
        return false;
    }
    bool ok = true;
    for (bool firefox : { false, true }) {
        for (const QString &path : manifestPaths(firefox)) {
            if (enabled)
                ok = writeFile(path, nativeHostManifest(hostExecutable(), firefox)) && ok;
            else
                QFile::remove(path);
        }
    }
    if (!ok && error)
        *error = QStringLiteral("Could not register Haul with every browser.");
    return ok;
}

#endif

QString browserExtensionDirectory()
{
    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList candidates = {
        appDir + QStringLiteral("/browser-extension"),
        appDir + QStringLiteral("/../share/haul/browser-extension"),
        QStringLiteral(HAUL_SOURCE_DIR "/browser"),
    };
    for (const QString &dir : candidates) {
        if (QFile::exists(dir + QStringLiteral("/manifest.json")))
            return QDir::cleanPath(dir);
    }
    return QString();
}

} // namespace haul::platform
