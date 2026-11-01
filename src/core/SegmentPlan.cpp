#include "SegmentPlan.h"

#include <algorithm>

namespace haul {

int segmentCountFor(qint64 totalBytes, int maxConnections)
{
    if (totalBytes <= 0 || maxConnections <= 1)
        return 1;
    const qint64 bySize = totalBytes / kMinSegmentSize;
    const qint64 count = std::min<qint64>({ static_cast<qint64>(maxConnections), bySize,
                                            static_cast<qint64>(kMaxSegments) });
    return static_cast<int>(std::max<qint64>(count, 1));
}

QVector<SegmentInfo> planSegments(qint64 totalBytes, int count)
{
    QVector<SegmentInfo> segments;
    if (totalBytes <= 0 || count <= 0 || count > totalBytes)
        return segments;

    const qint64 size = totalBytes / count;
    for (int i = 0; i < count; ++i) {
        SegmentInfo segment;
        segment.index = i;
        segment.start = i * size;
        segment.end = (i == count - 1) ? totalBytes - 1 : (i + 1) * size - 1;
        segments.append(segment);
    }
    return segments;
}

bool segmentsCoverExactly(const QVector<SegmentInfo> &segments, qint64 totalBytes)
{
    if (segments.isEmpty() || totalBytes <= 0)
        return false;
    qint64 expectedStart = 0;
    for (int i = 0; i < segments.size(); ++i) {
        const SegmentInfo &s = segments[i];
        if (s.index != i || s.start != expectedStart || s.end < s.start)
            return false;
        if (s.downloaded < 0 || s.downloaded > s.length())
            return false;
        expectedStart = s.end + 1;
    }
    return expectedStart == totalBytes;
}

qint64 segmentsDownloaded(const QVector<SegmentInfo> &segments)
{
    qint64 sum = 0;
    for (const SegmentInfo &s : segments)
        sum += std::min(s.downloaded, s.length());
    return sum;
}

} // namespace haul
