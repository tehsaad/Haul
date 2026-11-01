// Tests for the single-instance guard, its command channel and the browser hand-off.

#include "TestSupport.h"

#include "app/InstanceGuard.h"
#include "platform/BrowserIntegration.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTest>
#include <QUuid>

#include <thread>

using namespace haul;

namespace {

QString uniqueName()
{
    return QStringLiteral("haul-test-") + QUuid::createUuid().toString(QUuid::Id128).left(12);
}

// Calls a blocking send on another thread so this thread's event loop can answer it.
bool sendInBackground(const IpcCommand &command, const QString &name, QString *error)
{
    bool ok = false;
    QString message;
    std::thread sender([&] { ok = InstanceGuard::send(command, &message, name, 2000); });
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 500 && !ok && message.isEmpty())
        QTest::qWait(20);
    sender.join();
    if (error)
        *error = message;
    return ok;
}

QByteArray frame(const QByteArray &body)
{
    const quint32 length = static_cast<quint32>(body.size());
    return QByteArray(reinterpret_cast<const char *>(&length), 4) + body;
}

} // namespace

class TestInstance : public QObject
{
    Q_OBJECT

private slots:
    void onlyOneCopyCanRun()
    {
        const QString name = uniqueName();
        InstanceGuard first(name);
        QVERIFY(first.tryBecomePrimary());
        InstanceGuard second(name);
        QVERIFY(!second.tryBecomePrimary());
    }

    void lockIsFreedWhenTheFirstCopyExits()
    {
        const QString name = uniqueName();
        {
            InstanceGuard first(name);
            QVERIFY(first.tryBecomePrimary());
        }
        InstanceGuard next(name);
        QVERIFY(next.tryBecomePrimary());
    }

    void commandsReachTheRunningCopy()
    {
        qRegisterMetaType<haul::IpcCommand>();
        const QString name = uniqueName();
        InstanceGuard guard(name);
        QVERIFY(guard.tryBecomePrimary());
        QSignalSpy received(&guard, &InstanceGuard::commandReceived);

        IpcCommand add;
        add.type = IpcCommand::Add;
        add.url = "https://example.com/a.zip";
        QVERIFY(sendInBackground(add, name, nullptr));
        QTRY_COMPARE(received.count(), 1);
        const IpcCommand got = received.first().first().value<IpcCommand>();
        QCOMPARE(got.type, IpcCommand::Add);
        QCOMPARE(got.url, add.url);
    }

    void badCommandsAreRefused()
    {
        const QString name = uniqueName();
        InstanceGuard guard(name);
        QVERIFY(guard.tryBecomePrimary());
        QSignalSpy received(&guard, &InstanceGuard::commandReceived);

        IpcCommand add;
        add.type = IpcCommand::Add;
        add.url = "file:///etc/passwd";
        QString error;
        QVERIFY(!sendInBackground(add, name, &error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(received.count(), 0);

        IpcCommand decoded;
        QVERIFY(!decodeIpcCommand("not json", &decoded, &error));
        QVERIFY(!decodeIpcCommand("{\"cmd\":\"delete-everything\"}", &decoded, &error));
    }

    void sendFailsWhenNothingIsRunning()
    {
        IpcCommand show;
        QString error;
        QVERIFY(!InstanceGuard::send(show, &error, uniqueName(), 200));
        QVERIFY(!error.isEmpty());
    }

    void onlyOurExtensionIsAccepted()
    {
        QVERIFY(platform::isAllowedBrowserCaller({ QStringLiteral("chrome-extension://%1/").arg(QLatin1String(platform::kChromeExtensionId)) }));
        QVERIFY(platform::isAllowedBrowserCaller({ "/path/manifest.json", QString::fromLatin1(platform::kFirefoxExtensionId) }));
        QVERIFY(!platform::isAllowedBrowserCaller({ "chrome-extension://aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa/" }));
        QVERIFY(!platform::isAllowedBrowserCaller({}));
    }

    void browserMessagesAlwaysAskFirst()
    {
        IpcCommand command;
        QString error;
        QVERIFY(platform::parseBrowserMessage("{\"type\":\"add\",\"url\":\"https://example.com/a.zip\",\"filename\":\"a.zip\"}", &command, &error));
        QVERIFY(command.confirm);
        QCOMPARE(command.fileName, QString("a.zip"));
        QVERIFY(platform::parseBrowserMessage("{\"type\":\"add\",\"url\":\"https://example.com/a\",\"referrer\":\"https://example.org/\"}", &command, &error));
        QCOMPARE(command.referrer, QString("https://example.org/"));
        QVERIFY(platform::parseBrowserMessage("{\"type\":\"add\",\"url\":\"https://example.com/a\",\"referrer\":\"file:///etc/passwd\"}", &command, &error));
        QVERIFY(command.referrer.isEmpty());
        QVERIFY(!platform::parseBrowserMessage("{\"type\":\"add\",\"url\":\"file:///etc/passwd\"}", &command, &error));
        QVERIFY(!platform::parseBrowserMessage("{\"type\":\"show\"}", &command, &error));
        QVERIFY(!platform::parseBrowserMessage("garbage", &command, &error));
    }

    void manifestsNameOnlyOurExtension()
    {
        const QJsonObject chrome = QJsonDocument::fromJson(platform::nativeHostManifest("/usr/bin/haul-native-host", false)).object();
        QCOMPARE(chrome["name"].toString(), QString::fromLatin1(platform::kNativeHostName));
        QCOMPARE(chrome["type"].toString(), QString("stdio"));
        QCOMPARE(chrome["allowed_origins"].toArray().size(), 1);
        const QJsonObject firefox = QJsonDocument::fromJson(platform::nativeHostManifest("/usr/bin/haul-native-host", true)).object();
        QCOMPARE(firefox["allowed_extensions"].toArray().first().toString(), QString::fromLatin1(platform::kFirefoxExtensionId));
    }

    // Runs the real haul-native-host executable the way a browser does.
    void nativeHostForwardsToRunningCopy()
    {
        qRegisterMetaType<haul::IpcCommand>();
        const QString name = uniqueName();
        InstanceGuard guard(name);
        QVERIFY(guard.tryBecomePrimary());
        QSignalSpy received(&guard, &InstanceGuard::commandReceived);

        QProcess host;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert("HAUL_INSTANCE_NAME", name);
        host.setProcessEnvironment(env);
        host.start(QStringLiteral(HAUL_NATIVE_HOST),
                   { QStringLiteral("chrome-extension://%1/").arg(QLatin1String(platform::kChromeExtensionId)) });
        QVERIFY(host.waitForStarted());
        host.write(frame("{\"type\":\"add\",\"url\":\"https://example.com/x.iso\"}"));
        host.closeWriteChannel();
        QTRY_COMPARE_WITH_TIMEOUT(received.count(), 1, 5000);
        if (host.state() != QProcess::NotRunning)
            QVERIFY(host.waitForFinished(5000));
        QCOMPARE(host.exitCode(), 0);
        const QByteArray out = host.readAllStandardOutput();
        QVERIFY(QJsonDocument::fromJson(out.mid(4)).object()["ok"].toBool());
        const IpcCommand got = received.first().first().value<IpcCommand>();
        QVERIFY(got.confirm);
        QCOMPARE(got.url, QString("https://example.com/x.iso"));
    }

    void nativeHostDescribesStreams()
    {
        testsupport::TestServer server;
        QVERIFY(server.start());
        auto ask = [&](const QString &path) {
            QProcess host;
            host.start(QStringLiteral(HAUL_NATIVE_HOST), { QString::fromLatin1(platform::kFirefoxExtensionId) });
            host.waitForStarted();
            const QByteArray message = "{\"type\":\"inspect\",\"url\":\"" + server.url(path).toUtf8() + "\"}";
            host.write(frame(message));
            host.closeWriteChannel();
            host.waitForFinished(15000);
            return QJsonDocument::fromJson(host.readAllStandardOutput().mid(4)).object();
        };
        QJsonObject answer = ask("/hls/master.m3u8");
        QVERIFY(answer["ok"].toBool());
        QCOMPARE(answer["kind"].toString(), QString("hls"));
        QCOMPARE(answer["variants"].toArray().first().toObject()["label"].toString(), QString("720p"));
        QCOMPARE(answer["children"].toArray().size(), 2);

        answer = ask("/hls/high.m3u8?enc=key");
        QVERIFY(!answer["ok"].toBool());
        QVERIFY(answer["encrypted"].toBool());

        // The same executable also checks ordinary files.
        QProcess host;
        host.start(QStringLiteral(HAUL_NATIVE_HOST), { QString::fromLatin1(platform::kFirefoxExtensionId) });
        host.waitForStarted();
        host.write(frame("{\"type\":\"verify\",\"url\":\"" + server.url("/m/a.opus").toUtf8() + "\"}"));
        host.closeWriteChannel();
        host.waitForFinished(15000);
        answer = QJsonDocument::fromJson(host.readAllStandardOutput().mid(4)).object();
        QVERIFY(answer["ok"].toBool());
        QCOMPARE(answer["format"].toString(), QString("opus"));
    }

    void nativeHostRefusesOtherExtensions()
    {
        QProcess host;
        host.start(QStringLiteral(HAUL_NATIVE_HOST), { "chrome-extension://evil/" });
        QVERIFY(host.waitForStarted());
        host.write(frame("{\"type\":\"add\",\"url\":\"https://example.com/x.iso\"}"));
        host.closeWriteChannel();
        if (host.state() != QProcess::NotRunning)
            QVERIFY(host.waitForFinished(5000));
        QCOMPARE(host.exitCode(), 1);
    }
};

QTEST_GUILESS_MAIN(TestInstance)
#include "tst_instance.moc"
