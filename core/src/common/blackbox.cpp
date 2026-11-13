// 【黑匣子】实现。所有 I/O 在独立线程，调用方只做一次 memcpy 入队。
#include "blackbox.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <thread>
#include <time.h>          // clock_gettime / localtime_r / timespec
#include <sys/stat.h>      // mkdir / stat（POSIX，宿主编译也要）
#include <unistd.h>

#if defined(__ANDROID__) || defined(__linux__)
#if __has_include(<unwind.h>)
#include <unwind.h>
#define YIEYU_HAVE_UNWIND 1
#endif
#endif

namespace {

constexpr size_t kMsgCap    = 2048;   // 单条消息上限（含入参）
constexpr size_t kStackCap  = 768;
constexpr size_t kTraceCap  = 40;
constexpr size_t kModCap    = 16;
constexpr size_t kQueueLen  = 128;    // 满了就丢，绝不阻塞（128×2.9KB≈370KB BSS）
constexpr long   kRotateMax = 5L * 1024 * 1024;   // 5MB
constexpr int    kKeepOld   = 3;                  // .1 .2 .3

struct Record {
  int64_t  tsMs;
  int      level;
  uint16_t msgLen;
  uint16_t stackLen;
  char     trace[kTraceCap];
  char     module[kModCap];
  char     msg[kMsgCap];
  char     stack[kStackCap];
};

Record            g_q[kQueueLen];
size_t            g_head = 0, g_tail = 0;      // 环形；head==tail 空
std::mutex        g_mu;
std::condition_variable g_cv;
std::thread       g_writer;
std::atomic<bool> g_run{false};
std::atomic<uint32_t> g_dropped{0};

char   g_dir[512] = {0};
int    g_isPublic = 0;
FILE*  g_fp = nullptr;
long   g_bytes = 0;
char   g_file[600] = {0};

thread_local char t_trace[kTraceCap] = {0};

void now_iso(char* out, size_t cap) {
  struct timespec ts{};
  clock_gettime(CLOCK_REALTIME, &ts);
  struct tm tmv{};
  time_t s = ts.tv_sec;
  localtime_r(&s, &tmv);
  char base[32];
  strftime(base, sizeof(base), "%Y-%m-%dT%H:%M:%S", &tmv);
  snprintf(out, cap, "%s.%03ld%+03ld:%02ld", base,
           (long)(ts.tv_nsec / 1000000L), (long)(tmv.tm_gmtoff / 3600),
           (long)((tmv.tm_gmtoff % 3600) / 60));
}

int ensure_dir(const char* d) {
  if (!d || !*d) return -1;
  char tmp[512];
  snprintf(tmp, sizeof(tmp), "%s", d);
  for (char* p = tmp + 1; *p; ++p) {
    if (*p == '/') { *p = 0; mkdir(tmp, 0770); *p = '/'; }
  }
  mkdir(tmp, 0770);
  struct stat st{};
  return (stat(tmp, &st) == 0 && S_ISDIR(st.st_mode)) ? 0 : -1;
}

void rotate_if_needed() {
  if (!g_fp || g_bytes < kRotateMax) return;
  fclose(g_fp);
  g_fp = nullptr;
  // 删最旧 → 依次后移
  char oldest[620], newer[620];
  snprintf(oldest, sizeof(oldest), "%s.%d", g_file, kKeepOld);
  remove(oldest);
  for (int i = kKeepOld - 1; i >= 1; --i) {
    snprintf(oldest, sizeof(oldest), "%s.%d", g_file, i);
    snprintf(newer, sizeof(newer), "%s.%d", g_file, i + 1);
    rename(oldest, newer);
  }
  char first[620];
  snprintf(first, sizeof(first), "%s.1", g_file);
  rename(g_file, first);
  g_fp = fopen(g_file, "ab");
  g_bytes = 0;
}

void write_record(const Record& r) {
  if (!g_fp) {
    g_fp = fopen(g_file, "ab");
    if (!g_fp) return;                       // 磁盘满/权限没了：静默放弃（不许崩）
    fseek(g_fp, 0, SEEK_END);
    g_bytes = ftell(g_fp);
  }
  char ts[48];
  now_iso(ts, sizeof(ts));

  const char* lv = (r.level == YIEYU_LOG_FATAL) ? "FATAL" : "ERROR";
  int n = fprintf(g_fp,
      "%s | %s | trace=%s | module=%s | %.*s\n",
      ts, lv, r.trace[0] ? r.trace : "-", r.module[0] ? r.module : "-",
      (int)r.msgLen, r.msg);
  if (n > 0) g_bytes += n;
  if (r.stackLen) {
    int m = fprintf(g_fp, "    stack: %.*s\n", (int)r.stackLen, r.stack);
    if (m > 0) g_bytes += m;
  }
  fflush(g_fp);                              // 保命优先：宁可慢一点也要落地
  rotate_if_needed();
}

void writer_loop() {
  for (;;) {
    Record r;
    {
      std::unique_lock<std::mutex> lk(g_mu);
      g_cv.wait(lk, [] { return !g_run.load() || g_head != g_tail; });
      if (!g_run.load() && g_head == g_tail) return;
      r = g_q[g_tail];
      g_tail = (g_tail + 1) % kQueueLen;
    }
    write_record(r);
  }
}

#if defined(YIEYU_HAVE_UNWIND)
struct FrameCtx { void** frames; int count; int max; };
_Unwind_Reason_Code unwind_cb(struct _Unwind_Context* ctx, void* arg) {
  FrameCtx* c = static_cast<FrameCtx*>(arg);
  if (c->count >= c->max) return _URC_END_OF_STACK;
  uintptr_t ip = _Unwind_GetIP(ctx);
  if (ip) c->frames[c->count++] = reinterpret_cast<void*>(ip);
  return _URC_NO_REASON;
}
int capture_stack(char* out, size_t cap) {
  void* frames[16];
  FrameCtx c{frames, 0, 16};
  _Unwind_Backtrace(unwind_cb, &c);
  if (!c.count) return 0;
  size_t off = 0;
  off += snprintf(out + off, cap - off, "frames=%d", c.count);
  for (int i = 0; i < c.count && off + 20 < cap; ++i) {
    off += snprintf(out + off, cap - off, " %p", frames[i]);
  }
  return (int)off;
}
#else
int capture_stack(char*, size_t) { return 0; }
#endif

const char* parse_module(const char* fmt, char* mod, size_t modCap) {
  mod[0] = 0;
  if (fmt && fmt[0] == '[') {
    const char* e = strchr(fmt, ']');
    if (e && (size_t)(e - fmt) < modCap) {
      size_t n = (size_t)(e - fmt - 1);
      if (n >= modCap) n = modCap - 1;
      memcpy(mod, fmt + 1, n);
      mod[n] = 0;
      return e + 1;                          // 跳过 "[13]"
    }
  }
  return fmt;
}

}  // namespace

extern "C" {

int bb_init(const char* publicDir, const char* fallbackDir) {
  g_isPublic = 0;
  if (publicDir && ensure_dir(publicDir) == 0) { g_isPublic = 1; snprintf(g_dir, sizeof(g_dir), "%s", publicDir); }
  else if (fallbackDir && ensure_dir(fallbackDir) == 0) { snprintf(g_dir, sizeof(g_dir), "%s", fallbackDir); }
  else return -1;

  snprintf(g_file, sizeof(g_file), "%s/yieyu_log.txt", g_dir);
  g_fp = fopen(g_file, "ab");
  if (g_fp) { fseek(g_fp, 0, SEEK_END); g_bytes = ftell(g_fp); }

  g_run.store(true);
  g_writer = std::thread(writer_loop);
  return g_isPublic ? 0 : 1;                 // 1 = 降级到私有目录
}

const char* bb_dir(void) { return g_dir; }
int bb_is_public(void) { return g_isPublic; }

void bb_new_trace(void) {
  struct timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  static std::atomic<uint32_t> seq{0};
  snprintf(t_trace, kTraceCap, "%08x%04x", (unsigned)(ts.tv_nsec / 1000),
           (unsigned)(seq.fetch_add(1) & 0xffff));
}
const char* bb_trace(void) { return t_trace; }
void bb_set_trace(const char* id) { snprintf(t_trace, kTraceCap, "%s", id ? id : ""); }

void bb_write(int level, const char* fmt, ...) {
  if (!g_run.load(std::memory_order_relaxed)) return;
  Record r{};
  struct timespec ts{};
  clock_gettime(CLOCK_REALTIME, &ts);
  r.tsMs = (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
  r.level = level;
  snprintf(r.trace, sizeof(r.trace), "%s", t_trace);

  char mod[kModCap];
  const char* body = parse_module(fmt, mod, sizeof(mod));
  snprintf(r.module, sizeof(r.module), "%s", mod);

  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(r.msg, kMsgCap - 1, body, ap);
  va_end(ap);
  r.msgLen = (uint16_t)((n < 0) ? 0 : (n >= (int)kMsgCap ? kMsgCap - 1 : n));

  r.stackLen = (uint16_t)capture_stack(r.stack, kStackCap);

  {
    std::lock_guard<std::mutex> lk(g_mu);
    size_t next = (g_head + 1) % kQueueLen;
    if (next == g_tail) { g_dropped.fetch_add(1, std::memory_order_relaxed); return; }
    g_q[g_head] = r;
    g_head = next;
  }
  g_cv.notify_one();
}

void bb_flush(void) {
  for (int i = 0; i < 200; ++i) {            // 最多等 2s，绝不无限阻塞
    { std::lock_guard<std::mutex> lk(g_mu); if (g_head == g_tail) break; }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  if (g_fp) fflush(g_fp);
}

int bb_export(char* out, size_t cap) {
  bb_flush();
  if (!g_dir[0]) return -1;
  char dst[560];
  snprintf(dst, sizeof(dst), "%s/exported", g_dir);
  ensure_dir(dst);
  // 把当前 + 三个历史文件拷过去（拿不到公共目录时就地导出，文件管理器也能进私有目录的少见场景不管）
  const char* names[4] = {"yieyu_log.txt", "yieyu_log.txt.1", "yieyu_log.txt.2", "yieyu_log.txt.3"};
  for (auto* nm : names) {
    char src[600], to[620];
    snprintf(src, sizeof(src), "%s/%s", g_dir, nm);
    snprintf(to, sizeof(to), "%s/%s", dst, nm);
    FILE* a = fopen(src, "rb");
    if (!a) continue;
    FILE* b = fopen(to, "wb");
    if (b) { char buf[8192]; size_t r; while ((r = fread(buf, 1, sizeof(buf), a)) > 0) fwrite(buf, 1, r, b); fclose(b); }
    fclose(a);
  }
  snprintf(out, cap, "%s", dst);
  return 0;
}

void bb_shutdown(void) {
  if (!g_run.exchange(false)) return;
  g_cv.notify_all();
  if (g_writer.joinable()) g_writer.join();
  if (g_fp) { fclose(g_fp); g_fp = nullptr; }
}

uint32_t bb_dropped(void) { return g_dropped.load(); }

}  // extern "C"
