#include "sha256.h"
#include <cstring>
#include <cstdio>

namespace yieyu {
namespace {
const uint32_t K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

inline uint32_t ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

void block(Sha256Ctx* c, const uint8_t* p) {
  uint32_t w[64];
  for (int i = 0; i < 16; ++i) {
    w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
           ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
  }
  for (int i = 16; i < 64; ++i) {
    const uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t a = c->h[0], b = c->h[1], cc = c->h[2], d = c->h[3];
  uint32_t e = c->h[4], f = c->h[5], g = c->h[6], h = c->h[7];
  for (int i = 0; i < 64; ++i) {
    const uint32_t S1 = ror(e, 6) ^ ror(e, 11) ^ ror(e, 25);
    const uint32_t ch = (e & f) ^ ((~e) & g);
    const uint32_t t1 = h + S1 + ch + K[i] + w[i];
    const uint32_t S0 = ror(a, 2) ^ ror(a, 13) ^ ror(a, 22);
    const uint32_t mj = (a & b) ^ (a & cc) ^ (b & cc);
    const uint32_t t2 = S0 + mj;
    h = g; g = f; f = e; e = d + t1;
    d = cc; cc = b; b = a; a = t1 + t2;
  }
  c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d;
  c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}
}  // namespace

void sha256_init(Sha256Ctx* c) {
  static const uint32_t init[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                                   0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
  for (int i = 0; i < 8; ++i) c->h[i] = init[i];
  c->total = 0;
  c->buflen = 0;
}

void sha256_update(Sha256Ctx* c, const void* data, size_t n) {
  const uint8_t* p = static_cast<const uint8_t*>(data);
  c->total += n;
  while (n > 0) {
    const size_t take = (64 - c->buflen) < n ? (64 - c->buflen) : n;
    std::memcpy(c->buf + c->buflen, p, take);
    c->buflen += take;
    p += take;
    n -= take;
    if (c->buflen == 64) { block(c, c->buf); c->buflen = 0; }
  }
}

void sha256_final(Sha256Ctx* c, uint8_t out[32]) {
  const uint64_t bits = c->total * 8ull;
  uint8_t pad[72];
  size_t padLen = (c->buflen < 56) ? (56 - c->buflen) : (120 - c->buflen);
  std::memset(pad, 0, sizeof(pad));
  pad[0] = 0x80u;
  for (int i = 0; i < 8; ++i) pad[padLen + i] = (uint8_t)((bits >> (56 - 8 * i)) & 0xFFu);
  sha256_update(c, pad, padLen + 8);
  for (int i = 0; i < 8; ++i) {
    out[i * 4]     = (uint8_t)((c->h[i] >> 24) & 0xFFu);
    out[i * 4 + 1] = (uint8_t)((c->h[i] >> 16) & 0xFFu);
    out[i * 4 + 2] = (uint8_t)((c->h[i] >> 8) & 0xFFu);
    out[i * 4 + 3] = (uint8_t)(c->h[i] & 0xFFu);
  }
}

void sha256_hex(const uint8_t in[32], char out[65]) {
  static const char* hex = "0123456789abcdef";
  for (int i = 0; i < 32; ++i) {
    out[i * 2] = hex[(in[i] >> 4) & 0x0Fu];
    out[i * 2 + 1] = hex[in[i] & 0x0Fu];
  }
  out[64] = 0;
}

int sha256_file(const char* path, uint8_t out[32]) {
  if (!path || !*path) return -1;
  std::FILE* f = std::fopen(path, "rb");
  if (!f) return -2;
  Sha256Ctx c;
  sha256_init(&c);
  uint8_t buf[16 * 1024];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) sha256_update(&c, buf, n);
  const int err = std::ferror(f);
  std::fclose(f);
  if (err) return -3;
  sha256_final(&c, out);
  return 0;
}
}  // namespace yieyu
