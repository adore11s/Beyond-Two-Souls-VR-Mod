#pragma once
#include <cstdint>

namespace b2s::bones {

// Standard CRC32 (poly 0xEDB88320), identical to the engine's bone-name hash FUN_140986110.
inline uint32_t Crc32(const char* s) {
    uint32_t c = 0xFFFFFFFFu;
    for (; *s; ++s) {
        c ^= static_cast<uint8_t>(*s);
        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

}  // namespace b2s::bones
