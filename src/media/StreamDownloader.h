#pragma once

#include "StreamTypes.h"
#include "core/RateLimiter.h"
#include "network/CurlEasy.h"

#include <atomic>
#include <functional>

namespace haul {

struct StreamDownloadRequest {
    QVector<StreamTrack> tracks;
    QString directory;      // pieces are stored here; finished ones survive a restart
    int connections = 4;    // pieces fetched at the same time
    int attemptsPerPiece = 3;
    int retryDelayMs = 500;
    NetworkOptions options;
    RateLimiter *limiter = nullptr;
};

struct StreamDownloadResult {
    TransferError error;
    qint64 bytes = 0;
    int piecesDone = 0;
    int piecesTotal = 0;
};

// (bytes on disk, pieces finished, pieces in total), called from the transfer thread.
using StreamProgressCallback = std::function<void(qint64 bytes, int done, int total)>;

// Path of one finished piece.
QString streamPiecePath(const QString &directory, int track, int index);
// Sum of the finished pieces in a stream folder (used when recovering after a restart).
qint64 streamBytesOnDisk(const QString &directory);

// Downloads every missing piece, several at a time. Each piece is written to a ".part" file and
// renamed only when complete, so a restart skips exactly the pieces that finished. A piece that
// fails is tried again a few times before the whole download reports an error.
StreamDownloadResult downloadStreamPieces(const StreamDownloadRequest &request, const std::atomic_bool &cancel,
                                          const StreamProgressCallback &onProgress);

} // namespace haul
