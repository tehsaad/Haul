#pragma once

#include <QString>

namespace haul {

enum class ErrorKind {
    None,
    Transient,   // worth retrying: timeouts, dropped connections, 5xx, 429
    Permanent,   // retrying will not help: 404, 403, bad URL, disk full, permissions
    Cancelled    // stopped by the user (pause or cancel)
};

struct TransferError {
    ErrorKind kind = ErrorKind::None;
    QString message;
    int httpStatus = 0;
    int retryAfterSeconds = -1; // from a Retry-After header, -1 if absent

    bool isError() const { return kind != ErrorKind::None; }
};

// Classifies an HTTP status code of a final (non-redirect) response.
ErrorKind classifyHttpStatus(int status);

// User-facing text for common HTTP errors ("404 Not Found: the file does not exist ...").
QString describeHttpStatus(int status);

// Bounded exponential backoff. Never retries forever.
struct RetryPolicy {
    bool enabled = true;
    int maxRetries = 3;
    int baseDelaySeconds = 5;
    int maxDelaySeconds = 300;

    // `attempt` is the number of retries already made (0 for the first failure).
    bool shouldRetry(const TransferError &error, int attempt) const;

    // Delay before retry number `attempt` (0-based): base * 2^attempt, capped,
    // or the server's Retry-After value (also capped) when it asked for longer.
    int delayForAttempt(int attempt, int retryAfterSeconds = -1) const;
};

} // namespace haul
