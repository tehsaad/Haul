#pragma once

// Helpers shared by the test executables: starts tests/server/test_server.py on a random
// port and reproduces the bytes it serves so downloads can be compared exactly.

#include <QByteArray>
#include <QCryptographicHash>
#include <QFile>
#include <QProcess>
#include <QString>
#include <QTest>

#include <algorithm>

namespace testsupport {

inline QByteArray expectedContent(qint64 size, int seed = 0)
{
    const int prime = 65521;
    QByteArray block(prime, '\0');
    for (int j = 0; j < prime; ++j)
        block[j] = static_cast<char>((j * 31 + seed) & 0xFF);
    QByteArray out;
    out.reserve(static_cast<int>(size));
    while (out.size() < size)
        out += block.left(static_cast<int>(std::min<qint64>(prime, size - out.size())));
    return out;
}

inline QString sha256Hex(const QByteArray &data)
{
    return QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
}

inline QByteArray readFile(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

class TestServer
{
public:
    bool start()
    {
        m_process.setProgram(QStringLiteral(HAUL_PYTHON));
        m_process.setArguments({ QStringLiteral("-I"), QStringLiteral(HAUL_TEST_SERVER) });
        m_process.start();
        if (!m_process.waitForStarted(5000))
            return false;
        if (!m_process.waitForReadyRead(10000))
            return false;
        const QByteArray line = m_process.readLine().trimmed();
        if (!line.startsWith("PORT "))
            return false;
        m_port = line.mid(5).toInt();
        return m_port > 0;
    }

    ~TestServer()
    {
        m_process.kill();
        m_process.waitForFinished(3000);
    }

    QString url(const QString &path) const
    {
        return QStringLiteral("http://127.0.0.1:%1%2").arg(m_port).arg(path);
    }

private:
    QProcess m_process;
    int m_port = 0;
};

} // namespace testsupport
