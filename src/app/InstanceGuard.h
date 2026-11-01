#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>

#include <memory>

class QLocalServer;
class QLockFile;

namespace haul {

// One request from another process (a second copy of Haul or the browser's native host).
struct IpcCommand {
    enum Type { Show, Add };
    Type type = Show;
    QString url;
    QString fileName;     // optional hint, sanitized later
    bool confirm = false; // true: open the Add dialog instead of starting straight away
};

QByteArray encodeIpcCommand(const IpcCommand &command);
// Rejects malformed JSON, unknown commands and any URL that fails validateDownloadUrl().
bool decodeIpcCommand(const QByteArray &line, IpcCommand *command, QString *error);

// Makes sure only one copy of Haul runs per user, so two copies can never write to the same
// database or the same files. The first copy takes a lock file and listens on a local socket
// that only this user can open (nothing is exposed to the network); later copies pass their
// request along and exit.
class InstanceGuard : public QObject
{
    Q_OBJECT
public:
    // `name` isolates test runs; leave it empty for the real application.
    explicit InstanceGuard(const QString &name = QString(), QObject *parent = nullptr);
    ~InstanceGuard() override;

    // True if this process is now the running copy.
    bool tryBecomePrimary();

    // Sends one command to the running copy and waits for its answer.
    static bool send(const IpcCommand &command, QString *error = nullptr, const QString &name = QString(),
                     int timeoutMs = 3000);

signals:
    void commandReceived(const haul::IpcCommand &command);

private:
    void acceptConnection();

    QString m_name;
    std::unique_ptr<QLockFile> m_lock;
    QLocalServer *m_server = nullptr;
};

} // namespace haul

Q_DECLARE_METATYPE(haul::IpcCommand)
