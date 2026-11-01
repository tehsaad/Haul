// haul-native-host: started by the browser for Haul's extension. It reads one native-messaging
// message from stdin (a 4-byte length, then JSON), hands the link to the running copy of Haul,
// starting Haul first if needed, writes one reply and exits. It never downloads anything itself.

#include "app/InstanceGuard.h"
#include "core/UrlValidator.h"
#include "network/HttpProbe.h"
#include "platform/BrowserIntegration.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>

#include <cstdint>
#include <cstdio>
#include <cstring>

#if defined(Q_OS_WIN)
#include <fcntl.h>
#include <io.h>
#endif

using namespace haul;

namespace {

constexpr std::uint32_t kMaxMessage = 64 * 1024;

bool readMessage(QByteArray *message)
{
    std::uint32_t length = 0;
    if (std::fread(&length, sizeof(length), 1, stdin) != 1 || length == 0 || length > kMaxMessage)
        return false;
    message->resize(static_cast<int>(length));
    return std::fread(message->data(), 1, length, stdin) == length;
}

void writeReply(const QJsonObject &o)
{
    const QByteArray body = QJsonDocument(o).toJson(QJsonDocument::Compact);
    const std::uint32_t length = static_cast<std::uint32_t>(body.size());
    std::fwrite(&length, sizeof(length), 1, stdout);
    std::fwrite(body.constData(), 1, length, stdout);
    std::fflush(stdout);
}

void reply(bool ok, const QString &error = QString())
{
    QJsonObject o;
    o["ok"] = ok;
    if (!ok)
        o["error"] = error;
    writeReply(o);
}

} // namespace

int main(int argc, char *argv[])
{
#if defined(Q_OS_WIN)
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    QCoreApplication app(argc, argv);

    QStringList arguments = app.arguments();
    arguments.removeFirst();
    if (!platform::isAllowedBrowserCaller(arguments)) {
        reply(false, QStringLiteral("This extension is not allowed to use Haul."));
        return 1;
    }

    QByteArray message;
    IpcCommand command;
    QString error;
    if (!readMessage(&message)) {
        reply(false, QStringLiteral("Bad message."));
        return 1;
    }
    QUrl inspectUrl;
    QByteArray referer;
    if (platform::isInspectMessage(message, &inspectUrl, &referer)) {
        const UrlCheck check = validateDownloadUrl(inspectUrl.toString());
        if (!check.ok || isFtpUrl(check.url)) {
            reply(false, check.ok ? QStringLiteral("Streams can only be downloaded over http or https.") : check.error);
            return 1;
        }
        curl_global_init(CURL_GLOBAL_DEFAULT);
        NetworkOptions options;
        options.login.referer = referer;
        writeReply(platform::describeStream(inspectStream(check.url, options)));
        curl_global_cleanup();
        return 0;
    }
    if (platform::isVerifyMessage(message, &inspectUrl, &referer)) {
        const UrlCheck check = validateDownloadUrl(inspectUrl.toString());
        if (!check.ok) {
            reply(false, check.error);
            return 1;
        }
        curl_global_init(CURL_GLOBAL_DEFAULT);
        NetworkOptions options;
        options.connectTimeoutSeconds = 15;
        options.login.referer = referer;
        writeReply(platform::describeFile(probeUrl(check.url, options)));
        curl_global_cleanup();
        return 0;
    }
    if (!platform::parseBrowserMessage(message, &command, &error)) {
        reply(false, error);
        return 1;
    }

    const QString socketName = qEnvironmentVariable("HAUL_INSTANCE_NAME"); // used by the tests
    if (!InstanceGuard::send(command, &error, socketName, 300)) {
#if defined(Q_OS_WIN)
        const QString haul = app.applicationDirPath() + QStringLiteral("/haul.exe");
#else
        const QString haul = app.applicationDirPath() + QStringLiteral("/haul");
#endif
        if (!socketName.isEmpty() || !QProcess::startDetached(haul, {})
            || !InstanceGuard::send(command, &error, socketName, 8000)) {
            reply(false, QStringLiteral("Could not reach Haul: %1").arg(error));
            return 1;
        }
    }
    reply(true);
    return 0;
}
