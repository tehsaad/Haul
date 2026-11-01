#include "RetryPolicy.h"

#include <algorithm>

namespace haul {

ErrorKind classifyHttpStatus(int status)
{
    if (status >= 200 && status < 300)
        return ErrorKind::None;
    if (status == 408 || status == 425 || status == 429)
        return ErrorKind::Transient;
    if (status >= 500 && status != 501 && status != 505)
        return ErrorKind::Transient;
    return ErrorKind::Permanent;
}

QString describeHttpStatus(int status)
{
    switch (status) {
    case 400: return QStringLiteral("HTTP 400 Bad Request: the server rejected the request.");
    case 401: return QStringLiteral("HTTP 401 Unauthorized: this file needs a login. Right-click it and choose Log in.");
    case 403: return QStringLiteral("HTTP 403 Forbidden: the server refused Haul. It may need a login or only accept browsers.");
    case 404: return QStringLiteral("HTTP 404 Not Found: the file does not exist at this address.");
    case 410: return QStringLiteral("HTTP 410 Gone: the file has been removed.");
    case 416: return QStringLiteral("HTTP 416 Range Not Satisfiable: the requested byte range is invalid.");
    case 429: return QStringLiteral("HTTP 429 Too Many Requests: the server is rate limiting.");
    case 500: return QStringLiteral("HTTP 500 Internal Server Error.");
    case 502: return QStringLiteral("HTTP 502 Bad Gateway.");
    case 503: return QStringLiteral("HTTP 503 Service Unavailable: the server is busy.");
    case 504: return QStringLiteral("HTTP 504 Gateway Timeout.");
    default: break;
    }
    return QStringLiteral("The server answered with HTTP %1.").arg(status);
}

bool RetryPolicy::shouldRetry(const TransferError &error, int attempt) const
{
    if (!enabled || error.kind != ErrorKind::Transient)
        return false;
    return attempt < maxRetries;
}

int RetryPolicy::delayForAttempt(int attempt, int retryAfterSeconds) const
{
    const int safeAttempt = std::clamp(attempt, 0, 16);
    long long delay = static_cast<long long>(baseDelaySeconds) << safeAttempt;
    delay = std::min<long long>(delay, maxDelaySeconds);
    if (retryAfterSeconds > delay)
        delay = std::min(retryAfterSeconds, maxDelaySeconds);
    return static_cast<int>(std::max<long long>(delay, 1));
}

} // namespace haul
