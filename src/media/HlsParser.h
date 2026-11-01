#pragma once

#include "StreamTypes.h"

#include <QHash>
#include <QUrl>

namespace haul {

struct HlsMaster {
    QVector<StreamVariant> variants;    // best quality first
    QHash<QString, QString> audioGroups; // group id -> URL of its audio playlist
    bool sessionEncrypted = false;      // EXT-X-SESSION-KEY: the whole presentation is encrypted
};

struct HlsMedia {
    QVector<MediaSegment> segments;
    bool live = false;      // no EXT-X-ENDLIST
    bool encrypted = false; // any EXT-X-KEY whose METHOD is not NONE
    bool fragmentedMp4 = false; // uses EXT-X-MAP
    QString extension;
};

bool isHlsPlaylist(const QByteArray &body);
bool isHlsMaster(const QByteArray &body);
// Both return false and set *error if the text is not a usable playlist.
bool parseHlsMaster(const QByteArray &body, const QUrl &base, HlsMaster *master, QString *error);
bool parseHlsMedia(const QByteArray &body, const QUrl &base, HlsMedia *media, QString *error);

} // namespace haul
