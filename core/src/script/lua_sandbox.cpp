// 【模块 7】脚本沙箱 —— 现在跑的是我们自己的 Lua 子集解释器（minilua.cpp），
// 不再从 GitHub clone LuaJIT、也不再链接任何第三方运行时。
//
// 保留的能力与约定（与旧实现逐条对齐，上层 JSB 不用改）：
//   · lua_sandbox_init(dir)：建目录、点火
//   · lua_sandbox_reload(dir,&loaded,&rejected)：扫描 .lua → 黑名单静态检查 → 载入
//   · lua_sandbox_call(script, fn, argv, argc, out, cap)：调全局函数，第一返回值字符串化
//   · lua_sandbox_banned_text(text,&which)：黑名单的**唯一实现**（tools/collect-lua.mjs 同源）
//
// 诚实的边界：这是 Lua **子集**（见 minilua.h 顶部列表）。不支持的语法会在加载时报错，
// 不会"假装跑通"；错误进 Yieyu_LastError 与黑匣子。
#include "lua_sandbox.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "../common/log.h"
#include "minilua.h"

namespace {

std::mutex g_mu;
std::string g_dir;
std::map<std::string, void*> g_vms;         // 脚本名 → VM（互相隔离）
std::map<std::string, std::string> g_src;   // 脚本名 → 源码（重载时比对用）
uint64_t g_limit = 20000000;
int g_boot_done = 0;

// 黑名单：静态拦掉"脚本不该碰的东西"。与 tools/collect-lua.mjs 的 BAN 列表同源。
const char* kBanned[] = {
    "os.execute", "os.exit", "io.open", "io.write", "package.loadlib",
    "debug.sethook", "require(", "dofile(", "loadfile(", "loadstring(",
};
constexpr int kBannedCount = (int)(sizeof(kBanned) / sizeof(kBanned[0]));

bool endsWith(const char* s, const char* suf) {
  const size_t a = strlen(s), b = strlen(suf);
  return a >= b && strcmp(s + a - b, suf) == 0;
}

}  // namespace

extern "C" {

int lua_sandbox_banned_text(const char* text, const char** which) {
  if (!text) return 0;
  for (int i = 0; i < kBannedCount; ++i) {
    if (strstr(text, kBanned[i]) != nullptr) {
      if (which) *which = kBanned[i];
      return 1;
    }
  }
  return 0;
}

void lua_sandbox_set_limit(uint64_t instr) { g_limit = instr > 0 ? instr : 20000000; }

int lua_sandbox_init(const char* scriptsDir) {
  std::lock_guard<std::mutex> lk(g_mu);
  g_dir = scriptsDir ? scriptsDir : "";
  g_boot_done = 1;
  return 0;
}

int lua_sandbox_reload(const char* dir, int* loaded, int* rejected) {
  std::lock_guard<std::mutex> lk(g_mu);
  if (loaded) *loaded = 0;
  if (rejected) *rejected = 0;
  if (dir && *dir) g_dir = dir;
  if (g_dir.empty()) return -1;

  DIR* d = opendir(g_dir.c_str());
  if (!d) return -2;
  std::map<std::string, std::string> found;
  struct dirent* e = nullptr;
  while ((e = readdir(d)) != nullptr) {
    if (!endsWith(e->d_name, ".lua")) continue;
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", g_dir.c_str(), e->d_name);
    FILE* f = fopen(path, "rb");
    if (!f) { if (rejected) ++*rejected; continue; }
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > 512 * 1024) { fclose(f); if (rejected) ++*rejected; continue; }
    std::string src((size_t)size, '\0');
    const size_t got = fread(&src[0], 1, (size_t)size, f);
    fclose(f);
    if (got != (size_t)size) { if (rejected) ++*rejected; continue; }
    src.resize(got);

    const char* which = nullptr;
    if (lua_sandbox_banned_text(src.c_str(), &which)) {
      LOGE("[7] 拒绝脚本 %s：命中黑名单 \"%s\"", e->d_name, which ? which : "?");
      if (rejected) ++*rejected;
      continue;
    }
    found[e->d_name] = src;
  }
  closedir(d);

  // 换掉旧的 VM
  for (auto& kv : g_vms) ml_free(kv.second);
  g_vms.clear();
  g_src.clear();

  for (auto& kv : found) {
    void* vm = ml_new();
    if (!vm) { if (rejected) ++*rejected; continue; }
    const int rc = ml_load(vm, kv.first.c_str(), kv.second.c_str(), kv.second.size());
    if (rc != 0) {
      LOGE("[7] 脚本 %s 载入失败 rc=%d：%s", kv.first.c_str(), rc, ml_error(vm));
      ml_free(vm);
      if (rejected) ++*rejected;
      continue;
    }
    g_vms[kv.first] = vm;
    g_src[kv.first] = kv.second;
    if (loaded) ++*loaded;
  }
  return 0;
}

int lua_sandbox_run(const char* scriptName, const char* input, char* out, int cap) {
  // 兼容旧接口：跑脚本里的 main(input) 或者直接返回值
  const char* argv[1] = {input};
  return lua_sandbox_call(scriptName, "main", argv, input ? 1 : 0, out, cap);
}

int lua_sandbox_call(const char* scriptName, const char* fn,
                     const char* const* argv, int argc, char* out, int cap) {
  std::lock_guard<std::mutex> lk(g_mu);
  if (out && cap > 0) out[0] = 0;
  if (!scriptName || !fn) return -1;
  auto it = g_vms.find(scriptName);
  if (it == g_vms.end()) {
    // 没指定对：只在恰好一个脚本时兜底
    if (g_vms.size() != 1) return -2;
    it = g_vms.begin();
  }
  const int rc = ml_call(it->second, fn, argv, argc, out, cap);
  if (rc != 0) {
    LOGE("[7] 调用 %s.%s 失败 rc=%d：%s", scriptName, fn, rc, ml_error(it->second));
    return rc == -2 ? -3 : -4;
  }
  return 0;
}

void lua_sandbox_shutdown(void) {
  std::lock_guard<std::mutex> lk(g_mu);
  for (auto& kv : g_vms) ml_free(kv.second);
  g_vms.clear();
  g_src.clear();
  g_boot_done = 0;
}

}  // extern "C"
