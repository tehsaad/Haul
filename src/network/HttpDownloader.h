#pragma once

#include "CurlEasy.h"
#include "core/RateLimiter.h"

#include <atomic>
#include <functional>

namespace haul {

// Receives (bytes on disk so far, total size or -1). Called from the transfer thread.
using ProgressCallback = std::function<void(qint64 downloaded, qint64 total)>;

struct SingleRequest {
    QUrl url;
    QString filePath;          // the ".haul-part" file
    qint64 expectedTotal = -1; // size seen by the probe, -1 if unknown
    QString etag;              // validators used for If-Range when resuming
    QString lastModified;
    bool resume = true;        // continue from the current size of filePath if possible
    NetworkOptions options;
    RateLimiter *limiter = nullptr;
};

struct TransferResult {
    TransferError error;
    qint64 fileSize = 0;        // bytes in filePath after the attempt
    qint64 totalBytes = -1;
    bool restartedFromZero = false; // the server ignored our range, so we started over
    bool discardPartial = false;    // the partial data must not be reused for resuming
};

// Downloads one URL over a single connection into `request.filePath`.
//
// Resume rules:
//  * "Range: bytes=N-" is sent with "If-Range: <validator>" when N > 0.
//  * A 206 is accepted only if its Content-Range starts at N and the total matches.
//  * A 200 means the server ignored the range (or the file changed): the file is
//    truncated and written again from the first byte — never blindly appended.
TransferResult downloadSingle(const SingleRequest &request, const std::atomic_bool &cancel,
                              const ProgressCallback &onProgress);

// Sleeps for `ms` milliseconds in short steps, returning early if `cancel` becomes true.
void sleepUnlessCancelled(qint64 ms, const std::atomic_bool &cancel);

} // namespace haul
