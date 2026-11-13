#include "integrity.h"
#include "sha256.h"
#include "log.h"

#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <link.h>

#if defined(__GNUC__) || defined(__clang__)
// [12] 汇编里实现（asm/neon_ct_compare.S）。弱符号：宿主测试 / 非 arm64 时不链接也能过。
extern "C" uint64_t yieyu_ct_equal(const uint8_t* a, const uint8_t* b, uint64_t n) __attribute__((weak));
#endif

namespace {
char g_path[512] = {0};

int find_cb(struct dl_phdr_info* info, size_t, void* user) {
  (void)user;
  const char* name = info->dlpi_name;
  if (name && std::strstr(name, "libyieyu_core.so")) {
    std::snprintf(g_path, sizeof(g_path), "%s", name);
    return 1;                      /* 非 0 = 停止遍历 */
  }
  return 0;
}

// 常数时间比较（没有汇编时的兜底：逐字节异或累积，不提前 return）
int ct_equal(const uint8_t* a, const uint8_t* b, size_t n) {
#if defined(__GNUC__) || defined(__clang__)
  if (yieyu_ct_equal) return (int)yieyu_ct_equal(a, b, (uint64_t)n);
#endif
  uint8_t acc = 0;
  for (size_t i = 0; i < n; ++i) acc |= (uint8_t)(a[i] ^ b[i]);
  return acc == 0 ? 1 : 0;
}
}  // namespace

extern "C" {

const char* yieyu_self_path(void) {
  if (!g_path[0]) {
    (void)dl_iterate_phdr(find_cb, nullptr);
    if (!g_path[0]) {
      /* 兜底：从 /proc/self/maps 里找（个别 ROM 的 dlpi_name 为空） */
      std::FILE* f = std::fopen("/proc/self/maps", "r");
      if (f) {
        char line[512];
        while (std::fgets(line, sizeof(line), f)) {
          const char* p = std::strstr(line, "/libyieyu_core.so");
          if (p) {
            size_t n = std::strlen(line);
            while (n && (line[n - 1] == '\n' || line[n - 1] == '\r' || line[n - 1] == ' ')) line[--n] = 0;
            std::snprintf(g_path, sizeof(g_path), "%s", p);
            break;
          }
        }
        std::fclose(f);
      }
    }
  }
  return g_path;
}

int yieyu_self_digest(char outHex[65]) {
  if (!outHex) return -1;
  outHex[0] = 0;
  const char* path = yieyu_self_path();
  if (!path || !*path) return -1;
  uint8_t dig[32];
  if (yieyu::sha256_file(path, dig) != 0) {
    LOGE("[11] 自校验：读不到自身 %s", path);
    return -2;
  }
  yieyu::sha256_hex(dig, outHex);
  return 0;
}

int yieyu_integrity_check(const char* expectHex) {
  if (!expectHex || std::strlen(expectHex) != 64) return -1;
  char mine[65];
  if (yieyu_self_digest(mine) != 0) return -1;
  uint8_t a[32], b[32];
  auto unhex = [](const char* s, uint8_t* out) {
    for (int i = 0; i < 32; ++i) {
      auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return 0;
      };
      out[i] = (uint8_t)((nib(s[i * 2]) << 4) | nib(s[i * 2 + 1]));
    }
  };
  unhex(mine, a);
  unhex(expectHex, b);
  if (!ct_equal(a, b, 32)) {
    LOGE("[11] 完整性校验不通过：磁盘上的 so 与构建期不一致（被改过？）");
    return 0;
  }
  return 1;
}

}  // extern "C"
