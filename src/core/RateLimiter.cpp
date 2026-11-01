#include "RateLimiter.h"

namespace haul {

qint64 RateLimiter::consume(qint64 bytes)
{
    const qint64 limit = m_limit.load();
    const auto now = Clock::now();
    if (limit <= 0) {
        m_windowStart = now;
        m_windowBytes = 0;
        return 0;
    }

    // Start a fresh measuring window every second so a limit change takes effect quickly.
    auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_windowStart).count();
    if (elapsedMs >= 1000) {
        m_windowStart = now;
        m_windowBytes = 0;
        elapsedMs = 0;
    }

    m_windowBytes += bytes;
    const qint64 allowedMs = (m_windowBytes * 1000) / limit;
    return allowedMs > elapsedMs ? allowedMs - elapsedMs : 0;
}

} // namespace haul
