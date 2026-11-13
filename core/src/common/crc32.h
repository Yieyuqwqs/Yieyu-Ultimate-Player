// 自研 CRC-32（IEEE 802.3，多项式 0xEDB88320），首次调用时建表。
// 为什么不用 zlib：本工程不引第三方二进制，4KB 的表自己生成更省心。
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace yieyu {
namespace crc_detail {
inline const uint32_t* table() {
  static uint32_t t[256];
  static bool built = false;
  if (!built) {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
      t[i] = c;
    }
    built = true;
  }
  return t;
}
}  // namespace crc_detail

inline uint32_t crc32_update(uint32_t seed, const void* data, size_t n) {
  const uint32_t* t = crc_detail::table();
  const uint8_t* p = static_cast<const uint8_t*>(data);
  uint32_t c = seed ^ 0xFFFFFFFFu;
  for (size_t i = 0; i < n; ++i) c = t[(c ^ p[i]) & 0xFFu] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}

inline uint32_t crc32(const void* data, size_t n) { return crc32_update(0u, data, n); }
}  // namespace yieyu
