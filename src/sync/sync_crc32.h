#pragma once
#include <cstddef>
#include <cstdint>

namespace msync {

// CRC32 (IEEE), table-free so it costs no flash or RAM table. Chainable: pass the previous result as `crc`.
inline uint32_t crc32Update(uint32_t crc, const uint8_t* data, size_t len) {
  crc = ~crc;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}

}  // namespace msync
