// [24] JSB 实现。这里只做"搬运与路由"，任何业务判定都必须在各自模块里。
#include "jsb.h"
#include <cstdio>
#include <cstdlib>
#include "../common/log.h"
#include "../pipeline/engine.h"
#include "../common/blackbox.h"
#include "../script/lua_sandbox.h"
#include <atomic>
#include <cstring>
#include <mutex>

namespace {
std::atomic<bool> g_inited{false};
std::mutex        g_postMu;        // 下行串行（Flutter 只有一条 UI 线程，锁代价可忽略）
char              g_filesDir[512] = {0};
char              g_lastError[256] = {0};
}  // namespace

extern "C" {

int Yieyu_Init(const char* filesDir, const char* abi, uint32_t flags) {
  if (g_inited.exchange(true)) return 0;
  std::snprintf(g_filesDir, sizeof(g_filesDir), "%s", filesDir ? filesDir : "");
  LOGI("JSB init abi=%s filesDir=%s flags=0x%x ver=%d", abi ? abi : "?", g_filesDir, flags, YIEYU_JSB_ABI);
  return yieyu::Engine::Instance().Boot(g_filesDir, abi ? abi : "");
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
  return yieyu::Engine::Instance().HandleCommand(fbs, len);
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
  std::snprintf(outDir, cap, "%s", bb_dir());
  return bb_dir()[0] ? 0 : -1;
}
int Yieyu_LogIsPublic(void) { return bb_is_public(); }
void Yieyu_SetTrace(const char* traceId) { bb_set_trace(traceId); }

int Yieyu_ReloadScripts(int* loaded, int* rejected) {
  // 目录跟着黑匣子走：公共目录可用就用公共目录，否则用私有目录
  char dir[600];
  std::snprintf(dir, sizeof(dir), "%s/scripts", bb_dir());
  const int rc = lua_sandbox_reload(dir, loaded, rejected);
  if (rc != 0) {
    bb_write(YIEYU_LOG_ERROR, "[7] 脚本目录不可用：%s", dir);
  }
  return rc;
}

int Yieyu_Version(void) { return YIEYU_JSB_ABI; }
const char* Yieyu_LastError(void) { return g_lastError; }
void Yieyu_Stats(char* out, size_t cap) {
  if (!out || cap == 0) return;
  yieyu::Engine::Instance().Stats(out, cap);
}

}  // extern "C"
