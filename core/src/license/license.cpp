// [10] 卡密验签（零依赖：用我们自己的 SHA-256/HMAC，直接链进 core，不再 dlopen Rust 库）
//
// 令牌：YIEYU2.<b64url(payload)>.<b64url(hmac-sha256)>
// hmac = HMAC-SHA256(KEY, "YIEYU2." + payload_b64)
// 与 rust/license-ring 的实现逐字节一致（那边保留作为独立校验/交叉验证）。
#include <stdint.h>
#include <string.h>
#include <string>
#include <vector>

#include "../common/sha256.h"

namespace {

// 与 rust/license-ring/src/lib.rs 的 HMAC_KEY 完全相同的 32 字节
const uint8_t kKey[32] = {
    0x59, 0x49, 0x45, 0x59, 0x55, 0x2d, 0x55, 0x4c, 0x54, 0x49, 0x4d, 0x41, 0x54, 0x45, 0x2d, 0x50,
    0x4c, 0x41, 0x59, 0x45, 0x52, 0x2d, 0x48, 0x4d, 0x41, 0x43, 0x2d, 0x4b, 0x45, 0x59, 0x2d, 0x31,
};

void hmac_sha256(const uint8_t* msg, size_t n, uint8_t out[32]) {
  uint8_t k[64];
  memset(k, 0, sizeof(k));
  memcpy(k, kKey, sizeof(kKey));    // 32 ≤ 64，直接铺
  yieyu::Sha256Ctx c;
  uint8_t pad[64];
  for (int i = 0; i < 64; ++i) pad[i] = (uint8_t)(k[i] ^ 0x36);
  yieyu::sha256_init(&c);
  yieyu::sha256_update(&c, pad, 64);
  yieyu::sha256_update(&c, msg, n);
  uint8_t inner[32];
  yieyu::sha256_final(&c, inner);
  for (int i = 0; i < 64; ++i) pad[i] = (uint8_t)(k[i] ^ 0x5c);
  yieyu::sha256_init(&c);
  yieyu::sha256_update(&c, pad, 64);
  yieyu::sha256_update(&c, inner, 32);
  yieyu::sha256_final(&c, out);
}

int b64url_val(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '-') return 62;
  if (c == '_') return 63;
  return -1;
}

bool b64url_decode(const char* s, size_t n, std::vector<uint8_t>* out) {
  uint32_t acc = 0;
  int bits = 0;
  for (size_t i = 0; i < n; ++i) {
    if (s[i] == '=') break;
    const int v = b64url_val(s[i]);
    if (v < 0) return false;
    acc = (acc << 6) | (uint32_t)v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out->push_back((uint8_t)(acc >> bits));
    }
  }
  return true;
}

// payload 里取整数字段（不引 JSON 库：手写足够）
bool json_u64(const std::string& s, const char* key, uint64_t* out) {
  const std::string pat = std::string("\"") + key + "\"";
  size_t i = s.find(pat);
  if (i == std::string::npos) return false;
  i = s.find(':', i);
  if (i == std::string::npos) return false;
  ++i;
  while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
  uint64_t v = 0;
  bool any = false;
  while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
    v = v * 10 + (uint64_t)(s[i] - '0');
    any = true;
    ++i;
  }
  if (!any) return false;
  *out = v;
  return true;
}

bool json_str(const std::string& s, const char* key, std::string* out) {
  const std::string pat = std::string("\"") + key + "\"";
  size_t i = s.find(pat);
  if (i == std::string::npos) return false;
  i = s.find(':', i);
  if (i == std::string::npos) return false;
  const size_t q1 = s.find('"', i);
  if (q1 == std::string::npos) return false;
  const size_t q2 = s.find('"', q1 + 1);
  if (q2 == std::string::npos) return false;
  *out = s.substr(q1 + 1, q2 - q1 - 1);
  return true;
}

}  // namespace

extern "C" {

// 同 Rust 版的返回码：0=OK -1=Malformed -2=BadPrefix -3=BadBase64 -4=BadSignature -5=Expired -6=DeviceMismatch
int yieyu_license_verify(const uint8_t* token, size_t len, uint64_t now, const uint8_t* fp, size_t fpLen) {
  if (!token || len == 0) return -1;
  const std::string t((const char*)token, len);
  const size_t d1 = t.find('.');
  if (d1 == std::string::npos) return -1;
  const size_t d2 = t.find('.', d1 + 1);
  if (d2 == std::string::npos) return -1;
  if (t.find('.', d2 + 1) != std::string::npos) return -1;
  const std::string prefix = t.substr(0, d1);
  if (prefix != "YIEYU2") return -2;
  const std::string payloadB64 = t.substr(d1 + 1, d2 - d1 - 1);
  const std::string sigB64 = t.substr(d2 + 1);

  std::vector<uint8_t> sig;
  if (!b64url_decode(sigB64.c_str(), sigB64.size(), &sig)) return -3;
  std::vector<uint8_t> payloadRaw;
  if (!b64url_decode(payloadB64.c_str(), payloadB64.size(), &payloadRaw)) return -3;
  const std::string payload((const char*)payloadRaw.data(), payloadRaw.size());

  const std::string signedText = "YIEYU2." + payloadB64;
  uint8_t want[32];
  hmac_sha256((const uint8_t*)signedText.data(), signedText.size(), want);
  if (sig.size() != 32) return -4;
  uint8_t diff = 0;
  for (int i = 0; i < 32; ++i) diff |= (uint8_t)(sig[(size_t)i] ^ want[i]);
  if (diff != 0) return -4;

  uint64_t exp = 0;
  if (json_u64(payload, "e", &exp) && exp != 0 && now > exp) return -5;
  std::string dev;
  if (json_str(payload, "d", &dev) && !dev.empty() && fp && fpLen > 0) {
    char hex[32] = {0};
    for (size_t i = 0; i < fpLen && i < 8; ++i) snprintf(hex + i * 2, 3, "%02x", fp[i]);
    if (dev != hex) return -6;
  }
  return 0;
}

int yieyu_license_has_pubkey(void) {
  for (int i = 0; i < 32; ++i) if (kKey[i] != 0) return 1;
  return 0;
}

}  // extern "C"
