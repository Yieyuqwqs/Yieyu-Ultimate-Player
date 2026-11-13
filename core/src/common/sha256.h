// 自研 SHA-256（FIPS 180-4）。唯一用途：模块 11 的 .so 自校验（比对走模块 12 的 NEON 常数时间）。
// 不追求速度，只要求与标准实现逐字节一致（对照表在 core/tests 与本机 node 镜像里验过）。
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace yieyu {
struct Sha256Ctx {
  uint32_t h[8];
  uint64_t total;
  uint8_t buf[64];
  size_t buflen;
};
void sha256_init(Sha256Ctx* c);
void sha256_update(Sha256Ctx* c, const void* data, size_t n);
void sha256_final(Sha256Ctx* c, uint8_t out[32]);     // out = 32 字节原始摘要
void sha256_hex(const uint8_t in[32], char out[65]);  // 小写十六进制 + '\0'
int  sha256_file(const char* path, uint8_t out[32]);  // 0=成功
}  // namespace yieyu
