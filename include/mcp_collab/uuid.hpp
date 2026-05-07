#pragma once

#include <string>
#include <random>
#include <format>

namespace mcp_collab {

inline std::string generate_uuid() {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    static thread_local bool seeded = false;
    if (!seeded) {
        rng.seed(std::random_device{}());
        seeded = true;
    }

    uint64_t a = rng();
    uint64_t b = rng();

    a = (a & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;
    b = (b & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;

    return std::format("{:08x}-{:04x}-{:04x}-{:04x}-{:012x}",
        static_cast<uint32_t>(a >> 32),
        static_cast<uint16_t>((a >> 16) & 0xFFFF),
        static_cast<uint16_t>(a & 0xFFFF),
        static_cast<uint16_t>(b >> 48),
        static_cast<uint64_t>(b & 0xFFFFFFFFFFFFULL));
}

}