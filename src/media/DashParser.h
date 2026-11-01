#pragma once

#include "StreamTypes.h"

#include <QUrl>

namespace haul {

struct DashRepresentation {
    QString id;
    bool isVideo = false; // false means audio
    bool isMuxed = false; // carries both audio and video
    bool encrypted = false;
    int width = 0;
    int height = 0;
    qint64 bandwidth = 0;
    QString codecs;
    QString extension;
    QVector<MediaSegment> segments;
};

struct DashManifest {
    bool live = false;
    int periods = 0;
    double duration = 0.0;
    QVector<DashRepresentation> representations;
};

bool isDashManifest(const QByteArray &body);
// Returns false and sets *error if the manifest is malformed or uses something unsupported.
bool parseDashManifest(const QByteArray &body, const QUrl &base, DashManifest *manifest, QString *error);

// Parses an ISO 8601 duration such as "PT1H2M3.5S" into seconds; negative if invalid.
double parseIsoDuration(const QString &text);

} // namespace haul
