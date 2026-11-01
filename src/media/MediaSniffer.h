#pragma once

#include <QByteArray>
#include <QString>

namespace haul {

struct SniffResult {
    enum Kind { Unknown, Video, Audio, Manifest, NotMedia };
    Kind kind = Unknown;
    QString format; // "mp4", "mkv", "mp3", ...; empty when unknown
};

// Identifies media from its first bytes, whatever the URL or headers claim.
// NotMedia means a web page, JSON or similar was sent in place of a file.
SniffResult sniffMedia(const QByteArray &head);

} // namespace haul
