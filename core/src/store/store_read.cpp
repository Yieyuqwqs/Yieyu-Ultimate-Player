// [19] 本地库只读半边（零依赖：直接链进 core，不再 dlopen Rust 库）
//
// 与 [20] 共用同一文件格式（[magic "YKV1"][klen][vlen][crc][key][val]），**只读**。
// 读盘只在 open 时发生一次，之后全部走内存索引（微秒级）。
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "../common/crc32.h"

namespace {
struct Entry { long off; uint32_t klen; uint32_t vlen; };
std::mutex g_mu;
std::map<std::string, Entry> g_idx;
std::string g_path;
}  // namespace

extern "C" {

int yieyu_store_open(const char* path) {
  std::lock_guard<std::mutex> lk(g_mu);
  g_idx.clear();
  g_path = path ? path : "";
  if (g_path.empty()) return -1;
  FILE* f = fopen(g_path.c_str(), "rb");
  if (!f) return -2;
  fseek(f, 0, SEEK_END);
  const long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (size <= 0) { fclose(f); return -3; }
  std::vector<uint8_t> buf((size_t)size);
  const size_t got = fread(buf.data(), 1, buf.size(), f);
  fclose(f);
  if (got != buf.size()) return -4;

  const size_t hdr = 14;
  size_t off = 0;
  while (off + hdr <= buf.size()) {
    uint32_t magic = 0, vlen = 0, crc = 0;
    uint16_t klen = 0;
    memcpy(&magic, &buf[off], 4);
    memcpy(&klen, &buf[off + 4], 2);
    memcpy(&vlen, &buf[off + 6], 4);
    memcpy(&crc, &buf[off + 10], 4);
    if (magic != 0x31564B59u || klen == 0 || klen > 512) break;   // "YKV1"
    const size_t end = off + hdr + klen + vlen;
    if (end > buf.size()) break;
    const uint8_t* key = &buf[off + hdr];
    const uint8_t* val = &buf[off + hdr + klen];
    uint32_t c = yieyu::crc32_update(0u, key, klen);
    if (vlen) c = yieyu::crc32_update(c, val, vlen);
    if (c != crc) break;
    if (vlen == 0) {
      g_idx.erase(std::string((const char*)key, klen));
    } else {
      Entry e{};
      e.off = (long)off;
      e.klen = klen;
      e.vlen = vlen;
      g_idx[std::string((const char*)key, klen)] = e;
    }
    off = end;
  }
  return 0;
}

int yieyu_store_count(void) {
  std::lock_guard<std::mutex> lk(g_mu);
  return (int)g_idx.size();
}

int yieyu_store_get(const char* key, uint8_t* out, int cap) {
  std::lock_guard<std::mutex> lk(g_mu);
  if (!key || !out || cap <= 0) return -1;
  auto it = g_idx.find(key);
  if (it == g_idx.end()) return -3;
  FILE* f = fopen(g_path.c_str(), "rb");
  if (!f) return -2;
  const Entry& e = it->second;
  fseek(f, e.off + 14 + (long)e.klen, SEEK_SET);
  const int n = (int)(e.vlen < (uint32_t)cap ? e.vlen : (uint32_t)cap);
  const size_t got = fread(out, 1, (size_t)n, f);
  fclose(f);
  return got == (size_t)n ? (int)e.vlen : -4;
}

void yieyu_store_close(void) {
  std::lock_guard<std::mutex> lk(g_mu);
  g_idx.clear();
  g_path.clear();
}

}  // extern "C"
