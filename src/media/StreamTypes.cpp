#include "StreamTypes.h"

namespace haul {

double StreamTrack::duration() const
{
    double total = 0.0;
    for (const MediaSegment &segment : segments)
        total += segment.duration;
    return total;
}

QString StreamVariant::label() const
{
    if (audioOnly)
        return bandwidth > 0 ? QStringLiteral("%1 kbps audio").arg(bandwidth / 1000) : QStringLiteral("Audio");
    if (height > 0)
        return QStringLiteral("%1p").arg(height);
    return bandwidth > 0 ? QStringLiteral("%1 kbps").arg(bandwidth / 1000) : QStringLiteral("Default");
}

} // namespace haul
