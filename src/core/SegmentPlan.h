#pragma once

#include "DownloadTask.h"

namespace haul {

constexpr qint64 kMinSegmentSize = 1024 * 1024; // never split into pieces smaller than 1 MiB
constexpr int kMaxSegments = 16;

// How many connections are worth using for a file of `totalBytes`.
int segmentCountFor(qint64 totalBytes, int maxConnections);

// Splits [0, totalBytes) into `count` contiguous, non-overlapping inclusive ranges.
// The last segment absorbs the remainder. Returns an empty vector for invalid input.
QVector<SegmentInfo> planSegments(qint64 totalBytes, int count);

// True when the segments exactly cover [0, totalBytes) in order with no gaps or overlaps.
bool segmentsCoverExactly(const QVector<SegmentInfo> &segments, qint64 totalBytes);

qint64 segmentsDownloaded(const QVector<SegmentInfo> &segments);

} // namespace haul
