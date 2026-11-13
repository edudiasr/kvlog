#pragma once

// CRC-32 (o mesmo polinômio do zlib e do PNG). Cada registro do log leva o seu,
// e é assim que a recuperação sabe onde uma escrita foi cortada no meio.

#include <array>
#include <cstddef>
#include <cstdint>

namespace kvlog {

inline std::uint32_t crc32(const void* data, std::size_t n, std::uint32_t crc = 0) {
    static const auto table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    crc = ~crc;
    const auto* p = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < n; ++i) crc = table[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
    return ~crc;
}

}  // namespace kvlog
