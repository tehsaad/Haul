#include "Checksum.h"

#include <QCryptographicHash>
#include <QFile>

namespace haul {

bool isValidSha256Hex(const QString &text)
{
    const QString trimmed = text.trimmed();
    if (trimmed.size() != 64)
        return false;
    for (const QChar c : trimmed) {
        const bool hex = (c >= QLatin1Char('0') && c <= QLatin1Char('9'))
            || (c.toLower() >= QLatin1Char('a') && c.toLower() <= QLatin1Char('f'));
        if (!hex)
            return false;
    }
    return true;
}

QString sha256OfFile(const QString &path, const std::atomic_bool *cancel)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return QString();

    QCryptographicHash hash(QCryptographicHash::Sha256);
    QByteArray buffer;
    while (!file.atEnd()) {
        if (cancel && cancel->load())
            return QString();
        buffer = file.read(1 << 20);
        if (buffer.isEmpty() && file.error() != QFile::NoError)
            return QString();
        hash.addData(buffer);
    }
    return QString::fromLatin1(hash.result().toHex());
}

} // namespace haul
