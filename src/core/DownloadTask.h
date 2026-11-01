#pragma once

#include "TaskState.h"

#include <QDateTime>
#include <QString>
#include <QVector>
#include <QtGlobal>

namespace haul {

// One byte range of a segmented download. `end` is inclusive, as in HTTP.
struct SegmentInfo {
    int index = 0;
    qint64 start = 0;
    qint64 end = 0;
    qint64 downloaded = 0;

    qint64 length() const { return end - start + 1; }
    bool isComplete() const { return downloaded >= length(); }
};

enum class Priority { Low = 0, Normal = 1, High = 2 };

QString priorityLabel(Priority priority);

enum class ChecksumStatus {
    NotChecked = 0, // no trusted checksum was supplied
    Verified = 1,   // SHA-256 matched the checksum the user supplied
    Mismatch = 2    // SHA-256 did not match: the file is not trustworthy
};

// Everything Haul knows about one download.
// This is a plain value type: it is copied into worker threads and into the database.
struct DownloadTask {
    qint64 id = 0;
    QString url;
    QString directory;
    QString fileName;
    bool autoName = false; // name guessed from the URL; may be replaced by the server's suggestion

    TaskState state = TaskState::Queued;
    Priority priority = Priority::Normal;
    int queuePosition = 0;

    qint64 totalBytes = -1; // -1 when the server did not report a size
    qint64 downloadedBytes = 0;

    // HTTP validators used to detect that the remote file changed between attempts.
    QString etag;
    QString lastModified;
    bool acceptRanges = false;

    int retryCount = 0;
    QString errorMessage;
    bool interrupted = false; // was downloading when the previous session ended

    QString expectedSha256;
    ChecksumStatus checksumStatus = ChecksumStatus::NotChecked;

    QDateTime createdAt;
    QDateTime startedAt;
    QDateTime completedAt;

    QVector<SegmentInfo> segments; // empty for single-connection downloads

    // Runtime-only values (not stored in the database).
    double speed = 0.0;      // bytes per second, smoothed
    bool startRequested = false; // "Start now": does not wait for the queue to be started
    bool startedByQueue = false;
    QDateTime retryAt;       // when a scheduled automatic retry may start

    QString finalPath() const;
    QString partPath() const;
    QString segmentPath(int index) const;
    int progressPercent() const; // -1 when the size is unknown
};

} // namespace haul
