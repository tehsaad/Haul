#pragma once

#include "StreamTypes.h"
#include "network/CurlEasy.h"

#include <atomic>

namespace haul {

// What a manifest offers, for showing quality choices.
struct StreamInfo {
    bool ok = false;
    TransferError error;
    StreamKind kind = StreamKind::File;
    QVector<StreamVariant> variants; // best first; encrypted ones are left out
    double duration = 0.0;
    QStringList childUrls; // playlists this one refers to: the same video, not separate finds
};

// What must be fetched for one chosen quality.
struct StreamPlan {
    bool ok = false;
    TransferError error;
    StreamKind kind = StreamKind::File;
    QString label;
    QVector<StreamTrack> tracks;
    qint64 estimatedBytes = -1;
    QString fingerprint; // same value means the same pieces in the same order
};

// By URL only; a server can still serve a manifest from any address (see inspectStream).
StreamKind streamKindForUrl(const QUrl &url);
QString streamKindKey(StreamKind kind);
StreamKind streamKindFromKey(const QString &key);
bool isLocalNetworkHost(const QString &host);

// Fetches and parses a manifest. A file that turns out not to be a manifest gives kind == File.
StreamInfo inspectStream(const QUrl &url, const NetworkOptions &options, const std::atomic_bool *cancel = nullptr);

// Empty variantId means the best quality.
StreamPlan resolveStreamPlan(const QUrl &url, StreamKind kind, const QString &variantId, const NetworkOptions &options,
                             const std::atomic_bool *cancel = nullptr);

} // namespace haul
