#pragma once

#include <QSet>
#include <QString>
#include <QUrl>

namespace haul {

// Turns any server- or user-supplied name into a safe single path component:
// no directory separators, no "..", no control or reserved characters, no Windows
// device names (CON, NUL, ...), and at most 200 characters. Never returns an empty string.
QString sanitizeFileName(const QString &name);

// Best guess at a file name from the last path segment of a URL.
QString fileNameFromUrl(const QUrl &url);

// Parses a Content-Disposition header value. Supports filename*=UTF-8''... (RFC 6266/5987)
// and filename="...". Returns an empty string if no name is present.
QString fileNameFromContentDisposition(const QString &header);

// Returns `name` if no file called `name` exists in `directory` and the name is not in
// `reserved`; otherwise "name (1).ext", "name (2).ext", ...
QString uniqueFileName(const QString &directory, const QString &name,
                       const QSet<QString> &reserved = {});

// Splits "archive.tar.gz" into ("archive", ".tar.gz") and "photo.jpg" into ("photo", ".jpg").
void splitExtension(const QString &name, QString *base, QString *extension);

// "Video", "Audio", "Archive", "Document", "Program", "Image" or "Other".
QString categoryForFileName(const QString &name);

} // namespace haul
