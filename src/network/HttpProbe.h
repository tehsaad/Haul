#pragma once

#include "CurlEasy.h"

#include <atomic>

namespace haul {

// What we learn about a URL before (or before resuming) a download.
struct ProbeResult {
    bool ok = false;
    TransferError error;
    int httpStatus = 0;
    qint64 totalSize = -1;      // -1 when the server does not say
    bool rangeSupported = false; // server answered a byte-range request with a valid 206
    QString etag;
    QString lastModified;
    QString suggestedName;      // already sanitized
    QString contentType;
    QString effectiveUrl;       // after redirects
};

// Sends "GET <url>" with "Range: bytes=0-0". A server that supports ranges answers 206 with
// a Content-Range that reveals the full size; a server that ignores ranges answers 200 and we
// stop reading after a few kilobytes. This is more reliable than HEAD, which many servers mishandle.
ProbeResult probeUrl(const QUrl &url, const NetworkOptions &options,
                     const std::atomic_bool *cancel = nullptr);

} // namespace haul
