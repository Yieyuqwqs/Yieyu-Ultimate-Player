// [20] 本地持久化实现：追加日志 + CRC 校验 + 断电截断 + 阈值压实。
// 线程模型：只有控制面（引擎/JSB 线程）调用；PCM/音频线程一行都不许碰这里。
#include "kv_store.h"

#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../common/crc32.h"
#include "../common/log.h"

namespace {

constexpr uint32_t kRecMagic  = 0x31564B59u;   /* "YKV1" */
constexpr size_t   kHeader    = 4 + 2 + 4 + 4; /* magic klen vlen crc */
constexpr size_t   kMaxKey    = 512;
constexpr uint32_t kMaxVal    = 8u << 20;
constexpr long long kCompact  = 8ll << 20;     /* 8MB 之后压实 */

struct Entry {
  long long off;
  uint32_t  klen;
  uint32_t  vlen;
};

// 写者只有一个模块，但调用者有两个线程（控制面 + 解码线程写播放进度）
std::mutex g_mu;
int  g_fd = -1;
int  g_count = 0;
std::string g_path;
std::unordered_map<std::string, Entry> g_idx;
char g_err[192] = {0};

void set_err(const char* what, int e) {
  std::snprintf(g_err, sizeof(g_err), "%s (errno=%d)", what, e);
}
const char* strerrno() { return std::strerror(errno); }

bool write_all(int fd, const void* p, size_t n) {
  const uint8_t* q = static_cast<const uint8_t*>(p);
  while (n > 0) {
    const ssize_t w = ::write(fd, q, n);
    if (w < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (w == 0) return false;
    q += (size_t)w;
    n -= (size_t)w;
  }
  return true;
}

// 一条完整记录写进 fd（append）。返回写入后的新偏移；<0 失败。
long long append_record(int fd, long long at, const char* key, uint32_t klen,
                        const void* val, uint32_t vlen) {
  uint8_t hdr[kHeader];
  uint32_t crc = 0;
  crc = yieyu::crc32_update(crc, key, klen);
  if (vlen && val) crc = yieyu::crc32_update(crc, val, vlen);
  std::memcpy(hdr, &kRecMagic, 4);
  std::memcpy(hdr + 4, &klen, 2);
  std::memcpy(hdr + 6, &vlen, 4);
  std::memcpy(hdr + 10, &crc, 4);
  if (::lseek(fd, (off_t)at, SEEK_SET) < 0) return -1;
  if (!write_all(fd, hdr, kHeader)) return -2;
  if (!write_all(fd, key, klen)) return -3;
  if (vlen && !write_all(fd, val, vlen)) return -4;
  return at + (long long)kHeader + (long long)klen + (long long)vlen;
}

int rebuild_index() {
  g_idx.clear();
  g_count = 0;
  struct stat st {};
  if (::fstat(g_fd, &st) != 0) { set_err("fstat", errno); return -1; }
  const long long end = (long long)st.st_size;
  long long off = 0, good = 0;
  uint8_t hdr[kHeader];
  std::vector<uint8_t> val;
  std::string key;
  while (off + (long long)kHeader <= end) {
    if (::pread(g_fd, hdr, kHeader, (off_t)off) != (ssize_t)kHeader) break;
    uint32_t magic = 0, vlen = 0, crc = 0;
    uint16_t klen = 0;
    std::memcpy(&magic, hdr, 4);
    std::memcpy(&klen, hdr + 4, 2);
    std::memcpy(&vlen, hdr + 6, 4);
    std::memcpy(&crc, hdr + 10, 4);
    if (magic != kRecMagic || klen == 0 || klen > kMaxKey || vlen > kMaxVal) break;
    const long long recEnd = off + (long long)kHeader + (long long)klen + (long long)vlen;
    if (recEnd > end) break;
    key.assign((size_t)klen, '\0');
    if (::pread(g_fd, &key[0], klen, (off_t)(off + (long long)kHeader)) != (ssize_t)klen) break;
    val.resize(vlen);
    if (vlen && ::pread(g_fd, val.data(), vlen, (off_t)(off + (long long)kHeader + klen)) != (ssize_t)vlen) break;
    uint32_t c = yieyu::crc32_update(0u, key.data(), key.size());
    if (vlen) c = yieyu::crc32_update(c, val.data(), val.size());
    if (c != crc) break;                      /* 坏记录 = 断电残留，停在这里 */
    if (vlen == 0) {
      if (g_idx.erase(key) > 0) --g_count;
    } else {
      if (g_idx.find(key) == g_idx.end()) ++g_count;
      Entry e {};
      e.off = off;
      e.klen = klen;
      e.vlen = vlen;
      g_idx[key] = e;
    }
    off = recEnd;
    good = recEnd;
  }
  if (good != end) {                          /* 截断半条记录，保证下一次追加是干净的 */
    (void)::ftruncate(g_fd, (off_t)good);
    LOGE("[20] 存储尾部有 %lld 字节残缺记录，已截断（断电残留）", end - good);
  }
  g_err[0] = 0;
  return 0;
}

// 把活记录搬到新文件，再原子替换 —— 压实期间崩溃最多丢最后一次压实。
int compact_locked() {
  const std::string tmp = g_path + ".tmp";
  const int fd = ::open(tmp.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) { set_err("open tmp", errno); return -1; }
  long long at = 0;
  std::vector<uint8_t> val;
  for (const auto& kv : g_idx) {
    val.resize(kv.second.vlen);
    if (::pread(g_fd, val.data(), kv.second.vlen,
                (off_t)(kv.second.off + (long long)kHeader + kv.second.klen)) != (ssize_t)kv.second.vlen) {
      ::close(fd);
      return -2;
    }
    at = append_record(fd, at, kv.first.data(), (uint32_t)kv.first.size(), val.data(), kv.second.vlen);
    if (at < 0) { ::close(fd); return -3; }
  }
  ::fsync(fd);
  ::close(fd);
  if (::rename(tmp.c_str(), g_path.c_str()) != 0) { set_err("rename", errno); return -4; }
  ::close(g_fd);
  g_fd = ::open(g_path.c_str(), O_RDWR);
  if (g_fd < 0) { set_err("reopen", errno); return -5; }
  return rebuild_index();
}

}  // namespace

extern "C" {

int kv_open(const char* dir) {
  std::lock_guard<std::mutex> lk(g_mu);
  if (!dir || !*dir) return -1;
  if (g_fd >= 0) return 0;
  g_path = std::string(dir) + "/yieyu.kv";
  g_fd = ::open(g_path.c_str(), O_RDWR | O_CREAT, 0600);
  if (g_fd < 0) { set_err("open", errno); LOGE("[20] 存储打不开：%s (%s)", g_path.c_str(), strerrno()); return -2; }
  if (rebuild_index() != 0) { ::close(g_fd); g_fd = -1; return -3; }
  return 0;
}

int kv_put(const char* key, const void* val, int len) {
  std::lock_guard<std::mutex> lk(g_mu);
  if (g_fd < 0 || !key || !*key || len < 0) return -1;
  const size_t klen = std::strlen(key);
  if (klen > kMaxKey || (uint32_t)len > kMaxVal) return -2;
  const long long at = ::lseek(g_fd, 0, SEEK_END);
  if (at < 0) { set_err("lseek", errno); return -3; }
  const long long end = append_record(g_fd, at, key, (uint32_t)klen, val, (uint32_t)len);
  if (end < 0) { set_err("append", errno); return -4; }
  Entry e {};
  e.off = at;
  e.klen = (uint32_t)klen;
  e.vlen = (uint32_t)len;
  if (g_idx.find(key) == g_idx.end()) ++g_count;
  g_idx[std::string(key)] = e;
  if (end > kCompact) (void)compact_locked();
  return 0;
}

int kv_get(const char* key, void* out, int cap) {
  std::lock_guard<std::mutex> lk(g_mu);
  if (g_fd < 0 || !key || !out || cap <= 0) return -1;
  auto it = g_idx.find(std::string(key));
  if (it == g_idx.end()) return -2;
  const Entry& e = it->second;
  const int n = (int)(e.vlen < (uint32_t)cap ? e.vlen : (uint32_t)cap);
  if (::pread(g_fd, out, (size_t)n, (off_t)(e.off + (long long)kHeader + e.klen)) != (ssize_t)n) return -3;
  return n;
}

int kv_del(const char* key) {
  std::lock_guard<std::mutex> lk(g_mu);
  if (g_fd < 0 || !key) return -1;
  auto it = g_idx.find(std::string(key));
  if (it == g_idx.end()) return -2;
  const long long at = ::lseek(g_fd, 0, SEEK_END);
  if (at < 0) return -3;
  const size_t klen = std::strlen(key);
  if (append_record(g_fd, at, key, (uint32_t)klen, nullptr, 0u) < 0) return -4;
  g_idx.erase(it);
  --g_count;
  return 0;
}

int kv_count(void) {
  std::lock_guard<std::mutex> lk(g_mu);
  return g_count;
}

int kv_flush(void) {
  std::lock_guard<std::mutex> lk(g_mu);
  return (g_fd >= 0 && ::fsync(g_fd) == 0) ? 0 : -1;
}

void kv_close(void) {
  std::lock_guard<std::mutex> lk(g_mu);
  if (g_fd < 0) return;
  (void)::fsync(g_fd);
  ::close(g_fd);
  g_fd = -1;
  g_idx.clear();
  g_count = 0;
}

const char* kv_path(void) { return g_path.c_str(); }   // 只读，不需要锁（init 之后不再变）
const char* kv_last_error(void) { return g_err; }

}  // extern "C"
