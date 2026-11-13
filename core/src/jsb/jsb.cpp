// [24] JSB 实现。这里只做"搬运与路由"，任何业务判定都必须在各自模块里。
#include "jsb.h"
#include <cstdio>
#include <cerrno>
#include <vector>
#include <cstdlib>
#include "../common/log.h"
#include "../pipeline/engine.h"
#include "../common/blackbox.h"
#include "../common/app_paths.h"
#include "../script/lua_sandbox.h"
#include "../net/stream_http.h"
#include <cstdlib>
#include <sys/stat.h>
#include <atomic>
#include <cstring>
#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace {
std::atomic<bool> g_inited{false};
std::mutex        g_postMu;        // 下行串行（Flutter 只有一条 UI 线程，锁代价可忽略）
char              g_lastError[256] = {0};

// 失败原因只给「开发者面板/诊断文件」看，绝不进正常界面（铁律：UI 静默）
void set_err(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(g_lastError, sizeof(g_lastError), fmt, ap);
  va_end(ap);
}
}  // namespace

extern "C" {

// [4] Zig 定长池：命令帧（≤4KB）从这里取，避开每条 UI 命令都碰 malloc。
// ★ 用**弱符号**声明：libyieyu_mem.a 没链进来、或 Zig 侧改名了，也不会链接失败，
//   运行时指针为 null → 自动退化成"直接用调用方缓冲"。接模块不能把构建搞红。
#if defined(YIEYU_HAVE_MEM) && (defined(__GNUC__) || defined(__clang__))
extern "C" {
int   yieyu_mem_init(unsigned int blocks_per_size) __attribute__((weak));
void* yieyu_mem_alloc(size_t size) __attribute__((weak));
void  yieyu_mem_free(void* p, size_t size) __attribute__((weak));
int   yieyu_mem_alive(void) __attribute__((weak));
}
#define YIEYU_MEM_READY (yieyu_mem_alloc != nullptr)
#else
#define YIEYU_MEM_READY (0)
#endif

int Yieyu_Init(const char* filesDir, const char* abi, uint32_t flags) {
  if (g_inited.exchange(true)) return 0;
  LOGI("JSB init abi=%s filesDir=%s flags=0x%x ver=%d", abi ? abi : "?", filesDir ? filesDir : "?", flags, YIEYU_JSB_ABI);
#ifdef YIEYU_HAVE_MEM
  if (yieyu_mem_init) yieyu_mem_init(512);   // 每档 512 块，启动时一次拿满
#endif
  return yieyu::Engine::Instance().Boot(filesDir, abi ? abi : "");
}

void Yieyu_Shutdown(void) {
  if (!g_inited.exchange(false)) return;
  yieyu::Engine::Instance().Shutdown();
  LOGI("JSB shutdown");
}

int Yieyu_Post(const uint8_t* fbs, size_t len) {
  if (!g_inited.load() || !fbs || len == 0) return -1;
  std::lock_guard<std::mutex> lk(g_postMu);
  bb_new_trace();                       // 贯穿这一次命令的整条链路

  // 命令帧落进 Zig 池（≤4KB）。池子拿不到就直用原缓冲 —— 绝不因为分配失败丢命令。
  const uint8_t* frame = fbs;
  void* slab = nullptr;
#ifdef YIEYU_HAVE_MEM
  if (len <= 4096 && YIEYU_MEM_READY) {
    slab = yieyu_mem_alloc(len);
    if (slab) {
      std::memcpy(slab, fbs, len);
      frame = static_cast<const uint8_t*>(slab);
    }
  }
#endif
  const int rc = yieyu::Engine::Instance().HandleCommand(frame, len);
#if defined(YIEYU_HAVE_MEM)
  if (slab && yieyu_mem_free) yieyu_mem_free(slab, len);
#endif
  return rc;
}

size_t Yieyu_Poll(uint8_t* out, size_t cap) {
  if (!g_inited.load() || !out || cap == 0) return 0;
  return yieyu::Engine::Instance().DrainEvents(out, cap);
}

int Yieyu_Snapshot_Get(Yieyu_Snapshot* out) {
  if (!out) return -1;
  std::memset(out, 0, sizeof(*out));
  return yieyu::Engine::Instance().Snapshot(out);
}

// ── ⑦ 播放控制（异步受理，解码/打开都在 player 的线程里）─────────────
int Yieyu_PlayUrl(const char* uri) {
  if (!g_inited.load()) return -1;
  bb_new_trace();
  // ★ 先清掉上一次脚本/导入留下的错误：否则 Yieyu_LastError() 会把
  //   "上一次导入失败"当成"这次播放失败"报给面板，排查直接跑偏。
  set_err("");
  return yieyu::Engine::Instance().PlayUrl(uri);
}

void Yieyu_Pause(void) { if (g_inited.load()) yieyu::Engine::Instance().PauseAudio(); }
void Yieyu_Resume(void) { if (g_inited.load()) yieyu::Engine::Instance().ResumeAudio(); }
void Yieyu_Stop(void) { if (g_inited.load()) yieyu::Engine::Instance().StopAudio(); }
int  Yieyu_SeekMs(int64_t ms) { return g_inited.load() ? yieyu::Engine::Instance().SeekMs(ms) : -1; }
void Yieyu_SetVolume(float v) { if (g_inited.load()) yieyu::Engine::Instance().SetVolume(v); }

// ── ⑧ [17] DSP 音效链（默认旁路）───────────────────────────────────
void Yieyu_SetDsp(int on) { if (g_inited.load()) yieyu::Engine::Instance().SetDsp(on != 0); }
void Yieyu_SetEqGain(int band, float db) {
  if (g_inited.load()) yieyu::Engine::Instance().SetEqGain(band, db);
}
int Yieyu_HttpProbe(const char* url, char* out, int cap) {
  if (!url || !*url) return -1;
  return yieyu_http_probe(url, out, cap);
}

void Yieyu_SetRate(int hz) { if (g_inited.load()) yieyu::Engine::Instance().SetRate(hz); }
void Yieyu_SetTone(int tube, int vinyl, int lpf, float drive) {
  if (g_inited.load()) yieyu::Engine::Instance().SetTone(tube != 0, vinyl != 0, lpf != 0, drive);
}

// ── ⑨ [5] 搜歌：只投递关键字，网络在引擎的搜歌线程里跑 ─────────────
int Yieyu_Search(const char* keyword) {
  if (!g_inited.load()) { set_err("核心未初始化，搜索不可用"); return -1; }
  if (!keyword || !*keyword) { set_err("搜索关键字为空"); return -1; }
  const int rc = yieyu::Engine::Instance().Search(keyword);
  if (rc != 0) set_err("搜索请求受理失败 rc=%d", rc);
  else set_err("");
  return rc;
}

// ── ⑩ [5] 点歌：拼内部 URI，直链解析交给解码线程 ─────────────────────
int Yieyu_PlaySong(const char* source, const char* musicId, const char* mid) {
  if (!g_inited.load()) return -1;
  bb_new_trace();
  const int rc = yieyu::Engine::Instance().PlaySong(source, musicId, mid);
  if (rc != 0) set_err("点歌受理失败 rc=%d", rc);
  else set_err("");
  return rc;
}

int  Yieyu_Service_Ping(void) { return g_inited.load() ? 1 : 0; }
void Yieyu_Service_OnNotificationAction(int actionId) {
  yieyu::Engine::Instance().OnSystemAction(actionId);
}

void Yieyu_Free(void* p) { std::free(p); }

int Yieyu_ExportLogs(char* outDir, size_t cap) {
  if (!outDir || cap == 0) return -1;
  return bb_export(outDir, cap);
}
int Yieyu_LogDir(char* outDir, size_t cap) {
  if (!outDir || cap == 0) return -1;
  std::snprintf(outDir, cap, "%s", yp_home());
  return yp_home()[0] ? 0 : -1;
}
int Yieyu_LogIsPublic(void) { return yp_is_public(); }
void Yieyu_SetTrace(const char* traceId) { bb_set_trace(traceId); }

int Yieyu_ImportScript(const char* name, const uint8_t* data, int len) {
  if (!name || !data || len <= 0) {
    set_err("参数非法：name=%s len=%d", name ? name : "(null)", len);
    return -1;
  }
  const size_t nlen = std::strlen(name);
  if (nlen < 5 || std::strcmp(name + nlen - 4, ".lua") != 0) {
    set_err("文件名不是 .lua：%s", name);
    return -1;
  }
  if (len > 512 * 1024) {
    set_err("脚本太大：%d 字节（上限 512KB）", len);
    return -4;
  }

  // ★ 必须补 NUL：data 是 Dart 传来的裸字节，没有结束符（之前 strstr 越界踩过）
  std::vector<char> src((size_t)len + 1);
  std::memcpy(src.data(), data, (size_t)len);
  src[(size_t)len] = 0;

  // 黑名单：**唯一实现**在 lua_sandbox（这里不再维护第二份数组）
  const char* which = nullptr;
  if (lua_sandbox_banned_text(src.data(), &which) != 0) {
    bb_write(YIEYU_LOG_ERROR, "[7] 拒绝导入 %s：触发沙箱黑名单 \"%s\"（%d 字节）",
             name, which ? which : "?", len);
    set_err("触发沙箱黑名单：%s", which ? which : "?");
    return -2;
  }

  // 脚本目录只有一个来源：app_paths（已在 init 时建好）
  char path[800];
  std::snprintf(path, sizeof(path), "%s/%s", yp_scripts(), name);

  FILE* f = std::fopen(path, "wb");
  if (!f) {
    const int e1 = errno;
    bb_write(YIEYU_LOG_ERROR, "[7] 导入失败：写盘打不开 %s（errno=%d）", path, e1);
    set_err("写盘被拒 errno=%d path=%s", e1, path);
    return -3;
  }
  const size_t wrote = std::fwrite(src.data(), 1, (size_t)len, f);
  std::fflush(f);
  std::fclose(f);
  if (wrote != (size_t)len) {
    bb_write(YIEYU_LOG_ERROR, "[7] 导入失败：只写进 %zu/%d 字节 → %s", wrote, len, path);
    set_err("只写进 %zu/%d 字节", wrote, len);
    return -3;
  }
  // 立刻回读一次：写成功但读不到（权限/加密目录）这种情况要当场暴露
  FILE* rf = std::fopen(path, "rb");
  if (!rf) {
    set_err("写完读不回来 path=%s", path);
    return -3;
  }
  std::fclose(rf);

  set_err("");
  int loaded = 0, rejected = 0;
  lua_sandbox_reload(yp_scripts(), &loaded, &rejected);
  bb_write(YIEYU_LOG_ERROR, "[7] 导入完成 %s：登记 %d 个、拒绝 %d 个", name, loaded, rejected);
  if (loaded == 0) {
    set_err("脚本写入成功但没能登记（loaded=0, rejected=%d）", rejected);
    return -2;
  }
  return 0;
}

int Yieyu_ReloadScripts(int* loaded, int* rejected) {
  const int rc = lua_sandbox_reload(yp_scripts(), loaded, rejected);
  if (rc != 0) {
    bb_write(YIEYU_LOG_ERROR, "[7] 脚本目录不可用：%s", yp_scripts());
  }
  return rc;
}

int Yieyu_ScriptCall(const char* script, const char* fn,
                     const char* a1, const char* a2, const char* a3,
                     char* out, int cap) {
  if (out && cap > 0) out[0] = 0;
  if (!script || !fn) return -1;
  const char* argv[3] = {a1, a2, a3};
  int argc = 0;
  while (argc < 3 && argv[argc] != nullptr) ++argc;
  bb_new_trace();
  const int rc = lua_sandbox_call(script, fn, argv, argc, out, cap);
  if (rc != 0) {
    set_err("脚本调用失败 rc=%d（%s.%s）", rc, script, fn);
    bb_write(YIEYU_LOG_ERROR, "[7] ScriptCall 失败 rc=%d %s.%s", rc, script, fn);
  } else {
    set_err("");
  }
  return rc;
}

int Yieyu_Version(void) { return YIEYU_JSB_ABI; }
const char* Yieyu_LastError(void) {
  if (g_lastError[0]) return g_lastError;
  // 脚本侧没有错误时，把播放器的失败原因给出去（面板/诊断文件用得上）
  static char s_playErr[256];
  yieyu::Engine::Instance().PlayerError(s_playErr, sizeof(s_playErr));
  return s_playErr;
}
void Yieyu_Stats(char* out, size_t cap) {
  if (!out || cap == 0) return;
  yieyu::Engine::Instance().Stats(out, cap);
#ifdef YIEYU_HAVE_MEM
  // 模块 4 是否真的活着（池子初始化失败也不会让 App 崩，这里只是如实报）
  const int mem = (yieyu_mem_alive != nullptr) ? yieyu_mem_alive() : 0;
  const size_t n = std::strlen(out);
  if (n + 12 < cap) std::snprintf(out + n, cap - n, " mem=%d", mem);
#endif
}


// ── [11] 防篡改自校验：期望摘要随 APK 资产下来（构建期由 tools/obfuscate.mjs 生成）
int Yieyu_IntegrityCheck(const char* expectHex) {
  if (!expectHex || !*expectHex) return -1;
  return yieyu::Engine::Instance().IntegrityCheck(expectHex);
}

int Yieyu_SelfDigest(char* out, size_t cap) {
  if (!out || cap == 0) return -1;
  std::snprintf(out, cap, "%s", yieyu::Engine::Instance().SelfDigest());
  return out[0] ? 0 : -1;
}

// ── [19]/[20] 存储两侧各自数到几个键（诊断面板看，正常界面不显示）
int Yieyu_KvCount(void) { return yieyu::Engine::Instance().KvCount(); }
int Yieyu_StoreCount(void) { return yieyu::Engine::Instance().StoreCount(); }

}  // extern "C"
