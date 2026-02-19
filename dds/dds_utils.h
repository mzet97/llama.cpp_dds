#pragma once

// dds_utils.h — shared utilities for DDS components.

#include <cstdint>
#include <cstdio>
#include <random>
#include <string>

namespace llama_dds {

/// Generate a UUID v4 string (e.g. "550e8400-e29b-41d4-a716-446655440000").
/// Thread-safe: each thread owns its own RNG state (thread_local).
inline std::string generate_uuid() {
    // NOTE: thread_local gives each thread its own PRNG state, avoiding locks
    //       and data races with no cost to the hot path.
    thread_local std::mt19937                            gen(std::random_device{}());
    thread_local std::uniform_int_distribution<uint32_t> dis;

    uint8_t bytes[16];
    for (int i = 0; i < 16; i += 4) {
        uint32_t val = dis(gen);
        bytes[i]     = (val >> 0) & 0xFF;
        bytes[i + 1] = (val >> 8) & 0xFF;
        bytes[i + 2] = (val >> 16) & 0xFF;
        bytes[i + 3] = (val >> 24) & 0xFF;
    }

    // Set version (4) and variant (RFC 4122)
    bytes[6] = (bytes[6] & 0x0F) | 0x40;
    bytes[8] = (bytes[8] & 0x3F) | 0x80;

    char buf[37];
    snprintf(buf, sizeof(buf), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", bytes[0],
             bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7], bytes[8], bytes[9], bytes[10],
             bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);

    return std::string(buf);
}

}  // namespace llama_dds
