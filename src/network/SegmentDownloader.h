#pragma once

#include "CurlEasy.h"
#include "core/DownloadTask.h"
#include "core/RateLimiter.h"

#include <atomic>
#include <functional>

namespace haul {

using SegmentProgressCallback = std::function<void(const QVector<SegmentInfo> &segments)>;
using SegmentPathFunction = std::function<QString(int index)>;

struct SegmentedRequest {
    QUrl url;
    qint64 totalBytes = 0;
    QVector<SegmentInfo> segments; // planned ranges; `downloaded` is re-read from the files
    SegmentPathFunction segmentPath;
    QString etag;
    QString lastModified;
    NetworkOptions options;
    RateLimiter *limiter = nullptr;
};

struct SegmentedResult {
    TransferError error;
    bool rangeUnsupported = false; // a server ignored a range request: fall back to one connection
    QVector<SegmentInfo> segments; // progress of each segment after the attempt
};

// Downloads every incomplete segment in parallel (one connection per segment, all driven by
// one libcurl "multi" handle on the calling thread). Each segment is written to its own file,
// and every response is checked: it must be a 206 whose Content-Range is exactly the range we
// asked for. Nothing overlaps because each connection can only write inside its own range.
SegmentedResult downloadSegments(const SegmentedRequest &request, const std::atomic_bool &cancel,
                                 const SegmentProgressCallback &onProgress);

// Concatenates the segment files, in order, into `outputPath`, verifying each file's size first
// and the final size afterwards. On success the segment files are deleted.
bool assembleSegments(const QVector<SegmentInfo> &segments, const SegmentPathFunction &segmentPath,
                      const QString &outputPath, qint64 totalBytes, const std::atomic_bool &cancel,
                      QString *error);

} // namespace haul
