#include "SpeedMeter.h"

namespace haul {

SpeedMeter::SpeedMeter(double smoothing, qint64 minIntervalMs)
    : m_smoothing(smoothing)
    , m_minIntervalMs(minIntervalMs)
{
}

void SpeedMeter::addSample(qint64 totalBytes, qint64 timestampMs)
{
    if (m_lastBytes < 0 || totalBytes < m_lastBytes) {
        // First sample, or the transfer restarted from zero.
        m_lastBytes = totalBytes;
        m_lastTime = timestampMs;
        return;
    }

    const qint64 elapsed = timestampMs - m_lastTime;
    if (elapsed < m_minIntervalMs)
        return;

    const double instant = static_cast<double>(totalBytes - m_lastBytes) * 1000.0 / elapsed;
    if (m_hasSpeed)
        m_speed = m_smoothing * instant + (1.0 - m_smoothing) * m_speed;
    else
        m_speed = instant;
    m_hasSpeed = true;
    m_lastBytes = totalBytes;
    m_lastTime = timestampMs;
}

void SpeedMeter::reset()
{
    m_lastBytes = -1;
    m_lastTime = 0;
    m_speed = 0.0;
    m_hasSpeed = false;
}

} // namespace haul
