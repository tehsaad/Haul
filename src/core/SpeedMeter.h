#pragma once

#include <QtGlobal>

namespace haul {

// Smooths the download speed with an exponential moving average so the number
// shown in the UI does not jump around with every network packet.
class SpeedMeter
{
public:
    explicit SpeedMeter(double smoothing = 0.3, qint64 minIntervalMs = 250);

    // Feed the total number of bytes received so far and the current time.
    void addSample(qint64 totalBytes, qint64 timestampMs);
    void reset();

    double bytesPerSecond() const { return m_speed; }
    bool hasMeasurement() const { return m_hasSpeed; }

private:
    double m_smoothing;
    qint64 m_minIntervalMs;
    qint64 m_lastBytes = -1;
    qint64 m_lastTime = 0;
    double m_speed = 0.0;
    bool m_hasSpeed = false;
};

} // namespace haul
