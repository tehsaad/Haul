#include "FileNameUtils.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QStringList>

namespace haul {

namespace {

constexpr int kMaxNameLength = 200;

bool isWindowsReservedName(const QString &base)
{
    static const QStringList reserved = {
        "CON", "PRN", "AUX", "NUL",
        "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
        "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"
    };
    return reserved.contains(base.toUpper());
}

} // namespace

QString sanitizeFileName(const QString &name)
{
    // Only keep the last path component, whichever separator the server used.
    QString cleaned = name;
    cleaned.replace(QLatin1Char('\\'), QLatin1Char('/'));
    const int slash = cleaned.lastIndexOf(QLatin1Char('/'));
    if (slash >= 0)
        cleaned = cleaned.mid(slash + 1);

    QString out;
    out.reserve(cleaned.size());
    for (const QChar c : cleaned) {
        if (c.category() == QChar::Other_Control || c.category() == QChar::Other_Format)
            continue;
        if (QStringLiteral("<>:\"|?*").contains(c))
            out += QLatin1Char('_');
        else
            out += c;
    }

    // Leading dots would create hidden files or "..": trailing dots and spaces are invalid on Windows.
    while (out.startsWith(QLatin1Char('.')) || out.startsWith(QLatin1Char(' ')))
        out.remove(0, 1);
    while (out.endsWith(QLatin1Char('.')) || out.endsWith(QLatin1Char(' ')))
        out.chop(1);

    QString base;
    QString extension;
    splitExtension(out, &base, &extension);
    if (isWindowsReservedName(base))
        out = QLatin1Char('_') + out;

    // Never let our own temporary suffixes be chosen as a final name.
    if (out.contains(QLatin1String(".haul-")))
        out.replace(QLatin1String(".haul-"), QLatin1String("_haul-"));

    if (out.size() > kMaxNameLength) {
        splitExtension(out, &base, &extension);
        if (extension.size() > 20)
            extension.clear();
        out = base.left(kMaxNameLength - extension.size()) + extension;
    }

    if (out.isEmpty())
        out = QStringLiteral("download");
    return out;
}

QString fileNameFromUrl(const QUrl &url)
{
    const QString path = url.path(QUrl::FullyDecoded);
    QString last = path.section(QLatin1Char('/'), -1);
    if (last.isEmpty())
        last = url.host();
    return sanitizeFileName(last);
}

QString fileNameFromContentDisposition(const QString &header)
{
    QString plainName;
    QString extendedName;

    const QStringList parts = header.split(QLatin1Char(';'));
    for (QString part : parts) {
        part = part.trimmed();
        const int eq = part.indexOf(QLatin1Char('='));
        if (eq <= 0)
            continue;
        const QString key = part.left(eq).trimmed().toLower();
        QString value = part.mid(eq + 1).trimmed();

        if (key == QLatin1String("filename*")) {
            // charset'language'percent-encoded-value
            const int first = value.indexOf(QLatin1Char('\''));
            const int second = first >= 0 ? value.indexOf(QLatin1Char('\''), first + 1) : -1;
            if (second > first) {
                const QString charset = value.left(first).toLower();
                const QByteArray decoded = QByteArray::fromPercentEncoding(value.mid(second + 1).toLatin1());
                extendedName = charset == QLatin1String("utf-8") ? QString::fromUtf8(decoded)
                                                                  : QString::fromLatin1(decoded);
            }
        } else if (key == QLatin1String("filename")) {
            if (value.size() >= 2 && value.startsWith(QLatin1Char('"')) && value.endsWith(QLatin1Char('"')))
                value = value.mid(1, value.size() - 2).replace(QLatin1String("\\\""), QLatin1String("\""));
            plainName = value;
        }
    }

    const QString chosen = !extendedName.isEmpty() ? extendedName : plainName;
    return chosen.isEmpty() ? QString() : sanitizeFileName(chosen);
}

void splitExtension(const QString &name, QString *base, QString *extension)
{
    static const QStringList doubleExtensions = { ".tar.gz", ".tar.bz2", ".tar.xz", ".tar.zst" };
    for (const QString &ext : doubleExtensions) {
        if (name.endsWith(ext, Qt::CaseInsensitive) && name.size() > ext.size()) {
            *base = name.left(name.size() - ext.size());
            *extension = name.right(ext.size());
            return;
        }
    }
    const int dot = name.lastIndexOf(QLatin1Char('.'));
    if (dot > 0) {
        *base = name.left(dot);
        *extension = name.mid(dot);
    } else {
        *base = name;
        extension->clear();
    }
}

QString uniqueFileName(const QString &directory, const QString &name, const QSet<QString> &reserved)
{
    const QDir dir(directory);
    auto taken = [&](const QString &candidate) {
        return reserved.contains(candidate) || QFileInfo::exists(dir.filePath(candidate))
            || QFileInfo::exists(dir.filePath(candidate + QStringLiteral(".haul-part")));
    };

    if (!taken(name))
        return name;

    QString base;
    QString extension;
    splitExtension(name, &base, &extension);
    for (int i = 1; i < 10000; ++i) {
        const QString candidate = QStringLiteral("%1 (%2)%3").arg(base).arg(i).arg(extension);
        if (!taken(candidate))
            return candidate;
    }
    return QStringLiteral("%1 (%2)%3").arg(base).arg(QDateTime::currentMSecsSinceEpoch()).arg(extension);
}

QString categoryForFileName(const QString &name)
{
    const QString lower = name.toLower();
    auto endsWithAny = [&](const QStringList &extensions) {
        for (const QString &ext : extensions) {
            if (lower.endsWith(ext))
                return true;
        }
        return false;
    };

    if (endsWithAny({ ".mp4", ".mkv", ".avi", ".mov", ".webm", ".m4v", ".flv", ".wmv", ".ts", ".mpeg", ".mpg", ".3gp", ".ogv" }))
        return QStringLiteral("Video");
    if (endsWithAny({ ".mp3", ".flac", ".wav", ".ogg", ".oga", ".m4a", ".aac", ".opus", ".wma" }))
        return QStringLiteral("Audio");
    if (endsWithAny({ ".zip", ".rar", ".7z", ".tar", ".gz", ".bz2", ".xz", ".zst", ".iso" }))
        return QStringLiteral("Archive");
    if (endsWithAny({ ".pdf", ".doc", ".docx", ".xls", ".xlsx", ".ppt", ".pptx", ".txt", ".odt", ".epub", ".csv" }))
        return QStringLiteral("Document");
    if (endsWithAny({ ".exe", ".msi", ".deb", ".rpm", ".appimage", ".dmg", ".apk", ".flatpak", ".snap" }))
        return QStringLiteral("Program");
    if (endsWithAny({ ".png", ".jpg", ".jpeg", ".gif", ".webp", ".svg", ".bmp" }))
        return QStringLiteral("Image");
    return QStringLiteral("Other");
}

} // namespace haul
