// [24] JSB 实现。这里只做"搬运与路由"，任何业务判定都必须在各自模块里。
#include "jsb.h"
#include <cstdio>
#include <cstdlib>
#include "../common/log.h"
#include "../pipeline/engine.h"
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

int Yieyu_Version(void) { return YIEYU_JSB_ABI; }
const char* Yieyu_LastError(void) { return g_lastError; }
void Yieyu_Stats(char* out, size_t cap) {
  if (!out || cap == 0) return;
  yieyu::Engine::Instance().Stats(out, cap);
}

}  // extern "C"
