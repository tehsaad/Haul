#pragma once

#include "core/DownloadTask.h"
#include "core/RetryPolicy.h"

#include <QString>
#include <QVector>

namespace haul {

// One piece of a stream, in playback order. A byte range is used by HLS BYTERANGE and DASH SegmentList.
struct MediaSegment {
    QString url;
    qint64 rangeStart = -1;
    qint64 rangeLength = -1;
    double duration = 0.0;
    bool isInit = false; // initialization data (fMP4/WebM headers), not playable time
};

// Everything that must be fetched for one output track.
struct StreamTrack {
    enum Role { Muxed, Video, Audio };
    Role role = Muxed;
    QString extension;   // "ts", "mp4", "m4a", "webm", ...
    QVector<MediaSegment> segments;

    double duration() const;
};

// One quality the user can choose.
struct StreamVariant {
    QString id;          // stable across manifest fetches, e.g. "720p-2500000"
    int width = 0;
    int height = 0;
    qint64 bandwidth = 0; // bits per second, as the manifest advertises
    QString codecs;
    QString url;         // HLS: this variant's media playlist
    QString audioGroup;  // HLS: EXT-X-MEDIA group holding its separate audio
    bool audioOnly = false;
    bool separateAudio = false;
    qint64 estimatedBytes = -1;

    QString label() const; // "1080p", or "128 kbps audio"
};

constexpr int kMaxStreamSegments = 200000;

const char *const kEncryptedStreamMessage =
    "This video is encrypted (DRM or a key-protected stream), so Haul can't download it.";
const char *const kLiveStreamMessage =
    "This is a live stream. Haul only downloads videos with a fixed length.";

} // namespace haul
