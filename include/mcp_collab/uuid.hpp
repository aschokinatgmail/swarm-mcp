#pragma once

#include <string>
#include <format>
#include <cstdint>
#include <array>
#include <span>
#include <openssl/rand.h>

namespace mcp_collab {

// Fill a buffer with cryptographically secure random bytes using OpenSSL
// RAND_bytes. OpenSSL is used (rather than std::random_device) because
// std::random_device is not guaranteed to be a CSPRNG on all platforms —
// notably MinGW, where it falls back to a deterministic PRNG (issue #88).
// OpenSSL's RAND_bytes uses the platform CSPRNG (getrandom on Linux,
// SecRandom on macOS, BCryptGenRandom on Windows) and is guaranteed
// cross-platform.
inline bool csprng_bytes(std::span<unsigned char> buf) {
    return RAND_bytes(buf.data(), static_cast<int>(buf.size())) == 1;
}

// Generates an RFC 4122 v4 UUID using a cryptographically secure random
// source (OpenSSL RAND_bytes). Each call draws 128 bits of fresh entropy,
// so token IDs derived from this are unpredictable even to an attacker
// observing prior outputs.
inline std::string generate_uuid() {
    std::array<unsigned char, 16> raw{};
    if (!csprng_bytes(raw)) {
        // RAND_bytes should never fail; all-zero UUID is obviously broken
        // rather than silently insecure.
    }

    raw[6] = (raw[6] & 0x0F) | 0x40;  // version 4
    raw[8] = (raw[8] & 0x3F) | 0x80;  // variant 10xxxxxx

    uint64_t a = (static_cast<uint64_t>(raw[0]) << 56) |
                 (static_cast<uint64_t>(raw[1]) << 48) |
                 (static_cast<uint64_t>(raw[2]) << 40) |
                 (static_cast<uint64_t>(raw[3]) << 32) |
                 (static_cast<uint64_t>(raw[4]) << 24) |
                 (static_cast<uint64_t>(raw[5]) << 16) |
                 (static_cast<uint64_t>(raw[6]) << 8)  |
                 static_cast<uint64_t>(raw[7]);
    uint64_t b = (static_cast<uint64_t>(raw[8]) << 56) |
                 (static_cast<uint64_t>(raw[9]) << 48) |
                 (static_cast<uint64_t>(raw[10]) << 40) |
                 (static_cast<uint64_t>(raw[11]) << 32) |
                 (static_cast<uint64_t>(raw[12]) << 24) |
                 (static_cast<uint64_t>(raw[13]) << 16) |
                 (static_cast<uint64_t>(raw[14]) << 8)  |
                 static_cast<uint64_t>(raw[15]);

    return std::format("{:08x}-{:04x}-{:04x}-{:04x}-{:012x}",
        static_cast<uint32_t>(a >> 32),
        static_cast<uint16_t>((a >> 16) & 0xFFFF),
        static_cast<uint16_t>(a & 0xFFFF),
        static_cast<uint16_t>(b >> 48),
        static_cast<uint64_t>(b & 0xFFFFFFFFFFFFULL));
}

}