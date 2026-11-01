#include "InstanceGuard.h"

#include "core/UrlValidator.h"

#include <QDir>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QStandardPaths>
#include <QThread>

namespace haul {

namespace {

constexpr qint64 kMaxMessage = 64 * 1024;

// A folder only this user can write to, shared by every process of this user.
QString privateDirectory()
{
#if defined(Q_OS_WIN)
    return QDir::tempPath(); // %TEMP% is per user
#else
    const QString runtime = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    return runtime.isEmpty() ? QDir::tempPath() : runtime;
#endif
}

QString socketName(const QString &name)
{
    const QString base = name.isEmpty() ? QStringLiteral("haul") : name;
#if defined(Q_OS_WIN)
    return base + QLatin1Char('-') + qEnvironmentVariable("USERNAME"); // named pipe
#else
    return QDir(privateDirectory()).filePath(base + QStringLiteral(".sock"));
#endif
}

} // namespace

QByteArray encodeIpcCommand(const IpcCommand &command)
{
    QJsonObject o;
    o["cmd"] = command.type == IpcCommand::Add ? QStringLiteral("add") : QStringLiteral("show");
    if (command.type == IpcCommand::Add) {
        o["url"] = command.url;
        if (!command.fileName.isEmpty())
            o["name"] = command.fileName;
        o["confirm"] = command.confirm;
    }
    return QJsonDocument(o).toJson(QJsonDocument::Compact) + '\n';
}

bool decodeIpcCommand(const QByteArray &line, IpcCommand *command, QString *error)
{
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    const QJsonDocument document = QJsonDocument::fromJson(line);
    if (!document.isObject())
        return fail(QStringLiteral("Not a valid message."));
    const QJsonObject o = document.object();
    const QString cmd = o.value("cmd").toString();
    if (cmd == QLatin1String("show")) {
        command->type = IpcCommand::Show;
        return true;
    }
    if (cmd != QLatin1String("add"))
        return fail(QStringLiteral("Unknown command."));

    const UrlCheck check = validateDownloadUrl(o.value("url").toString());
    if (!check.ok)
        return fail(check.error);
    command->type = IpcCommand::Add;
    command->url = check.url.toString(QUrl::FullyEncoded);
    command->fileName = o.value("name").toString();
    command->confirm = o.value("confirm").toBool();
    return true;
}

InstanceGuard::InstanceGuard(const QString &name, QObject *parent)
    : QObject(parent)
    , m_name(name)
{
}

InstanceGuard::~InstanceGuard() = default;

bool InstanceGuard::tryBecomePrimary()
{
    const QString base = m_name.isEmpty() ? QStringLiteral("haul") : m_name;
    m_lock = std::make_unique<QLockFile>(QDir(privateDirectory()).filePath(base + QStringLiteral(".lock")));
    m_lock->setStaleLockTime(0); // the lock is held for the whole run; only a dead owner frees it
    if (!m_lock->tryLock(0))
        return false;

    const QString name = socketName(m_name);
    QLocalServer::removeServer(name); // left behind if the last run crashed; we own the lock now
    m_server = new QLocalServer(this);
    m_server->setSocketOptions(QLocalServer::UserAccessOption);
    connect(m_server, &QLocalServer::newConnection, this, &InstanceGuard::acceptConnection);
    if (!m_server->listen(name))
        qWarning("Haul could not open its local socket: %s", qPrintable(m_server->errorString()));
    return true;
}

void InstanceGuard::acceptConnection()
{
    while (QLocalSocket *socket = m_server->nextPendingConnection()) {
        connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
        connect(socket, &QLocalSocket::readyRead, this, [this, socket] {
            if (socket->bytesAvailable() > kMaxMessage) {
                socket->abort();
                return;
            }
            if (!socket->canReadLine())
                return;
            IpcCommand command;
            QString error;
            if (decodeIpcCommand(socket->readLine(kMaxMessage), &command, &error)) {
                socket->write("ok\n");
                emit commandReceived(command);
            } else {
                socket->write("error " + error.toUtf8() + '\n');
            }
            socket->flush();
        });
    }
}

bool InstanceGuard::send(const IpcCommand &command, QString *error, const QString &name, int timeoutMs)
{
    // The running copy may have its lock but not its socket yet, so keep trying briefly.
    QLocalSocket socket;
    QElapsedTimer timer;
    timer.start();
    while (true) {
        socket.connectToServer(socketName(name));
        if (socket.waitForConnected(500))
            break;
        if (timer.elapsed() >= timeoutMs) {
            if (error)
                *error = QStringLiteral("Haul is not running.");
            return false;
        }
        socket.abort();
        QThread::msleep(100);
    }

    socket.write(encodeIpcCommand(command));
    socket.flush();
    while (!socket.canReadLine()) {
        if (!socket.waitForReadyRead(timeoutMs)) {
            if (error)
                *error = QStringLiteral("Haul did not answer.");
            return false;
        }
    }
    const QByteArray answer = socket.readLine().trimmed();
    if (answer == "ok")
        return true;
    if (error)
        *error = QString::fromUtf8(answer.mid(6));
    return false;
}

} // namespace haul
