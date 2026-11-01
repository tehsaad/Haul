#pragma once

#include <QString>

#include <atomic>

namespace haul {

// True for exactly 64 hexadecimal characters (case-insensitive).
bool isValidSha256Hex(const QString &text);

// Computes the SHA-256 of a file as lowercase hex. Returns an empty string if the file
// cannot be read or `cancel` becomes true while hashing.
QString sha256OfFile(const QString &path, const std::atomic_bool *cancel = nullptr);

} // namespace haul
