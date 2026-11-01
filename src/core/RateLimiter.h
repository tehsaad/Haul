#pragma once

#include <QtGlobal>

#include <atomic>
#include <chrono>

namespace haul {

// A small token-bucket limiter used from inside a transfer thread.
// The limit can be changed from the GUI thread at any time (it is atomic),
// which lets Haul re-balance the global bandwidth cap while downloads run.
class RateLimiter
{
public:
    // 0 means unlimited.
    void setLimit(qint64 bytesPerSecond) { m_limit.store(bytesPerSecond); }
    qint64 limit() const { return m_limit.load(); }

    // Accounts for `bytes` just received and returns how long the caller
    // should sleep (in milliseconds) to stay under the limit.
    qint64 consume(qint64 bytes);

private:
    using Clock = std::chrono::steady_clock;
    std::atomic<qint64> m_limit { 0 };
    Clock::time_point m_windowStart = Clock::now();
    qint64 m_windowBytes = 0;
};

} // namespace haul
