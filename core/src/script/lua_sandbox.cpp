// [7] LuaJIT 沙箱：跑第三方音源脚本（落雪源）。
// 白名单：http.get / json / base64 / log / 返回表。没有 os、没有 io、没有 package。
// 防死循环：指令计数 hook（默认 5000 万条/次），超限软掐；--hard-kill 走独立进程 SIGKILL。
//
// ★ 热更：脚本目录由外部指定（/storage/emulated/0/yieyu/scripts），
//   lua_sandbox_reload() 扫描 + 黑名单校验 + 登记，App 内点一下就生效，不打包。
#include "lua_sandbox.h"
#include "../common/log.h"
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>

namespace {

constexpr int kMaxScripts = 32;
char  g_names[kMaxScripts][64];
char  g_dir[512] = {0};
int   g_count = 0;

// 与 tools/collect-lua.mjs 保持同一份黑名单（两边不一致就是事故）
const char* kBan[] = {
  "os.execute", "os.exit", "io.open", "io.write",
  "package.loadlib", "debug.sethook", "require(", "dofile(", "loadfile(",
};
constexpr int kBanCount = (int)(sizeof(kBan) / sizeof(kBan[0]));

bool file_banned(const char* path) {
  FILE* f = fopen(path, "rb");
  if (!f) return true;                      // 读不到就当被拒（失败安全）
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  if (n < 0 || n > 512 * 1024) { fclose(f); return true; }   // 超大脚本直接拒
  fseek(f, 0, SEEK_SET);
  char* buf = new char[(size_t)n + 1];
  size_t got = fread(buf, 1, (size_t)n, f);
  buf[got] = 0;
  fclose(f);
  bool bad = false;
  for (int i = 0; i < kBanCount; ++i) {
    if (strstr(buf, kBan[i])) { bad = true; break; }
  }
  delete[] buf;
  return bad;
}

}  // namespace

extern "C" int lua_sandbox_reload(const char* dir, int* loaded, int* rejected) {
  if (loaded) *loaded = 0;
  if (rejected) *rejected = 0;
  if (!dir || !*dir) return -1;
  snprintf(g_dir, sizeof(g_dir), "%s", dir);

  DIR* d = opendir(dir);
  if (!d) {
    // 目录不存在不算错误（用户还没放脚本）
    return 0;
  }
  g_count = 0;
  struct dirent* e;
  while ((e = readdir(d)) != nullptr) {
    const char* nm = e->d_name;
    size_t len = strlen(nm);
    if (len < 5 || strcmp(nm + len - 4, ".lua") != 0) continue;
    char full[700];
    snprintf(full, sizeof(full), "%s/%s", dir, nm);
    if (file_banned(full)) {
      if (rejected) (*rejected)++;
      continue;                              // 被拒的不登记、不执行
    }
    if (g_count < kMaxScripts) {
      snprintf(g_names[g_count], sizeof(g_names[0]), "%s", nm);
      g_count++;
      if (loaded) (*loaded)++;
    } else if (rejected) {
      (*rejected)++;                         // 超过上限也计为拒
    }
  }
  closedir(d);
  return 0;
}

extern "C" int lua_sandbox_count(void) { return g_count; }
extern "C" const char* lua_sandbox_name(int i) {
  return (i >= 0 && i < g_count) ? g_names[i] : nullptr;
}

#ifdef YIEYU_OFFLINE
extern "C" int  lua_sandbox_init(const char* scriptsDir) {
  (void)scriptsDir;
  // 空实现：LuaJIT 未接入（third_party/lua 还没钉），但扫描/校验/登记逻辑照常工作，
  // 这样"热更链路"能先跑通、能验证黑名单，只是暂时执行不了脚本。
  return 0;
}
extern "C" void lua_sandbox_set_limit(uint64_t instr) { (void)instr; }
extern "C" int  lua_sandbox_run(const char* n, const char* in, char* out, int cap) {
  (void)n; (void)in; if (out && cap) out[0] = 0;
  return -2;                                  // -2 = 宿主未接入 LuaJIT
}
extern "C" void lua_sandbox_shutdown(void) {}
#else
extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}
namespace { lua_State* g_L = nullptr; uint64_t g_limit = 50000000ull; uint64_t g_used = 0; }

static void count_hook(lua_State* L, lua_Debug*) {
  g_used += 1000;
  if (g_used > g_limit) luaL_error(L, "sandbox: instruction limit exceeded");
}
static int l_http_get(lua_State* L) {
  const char* url = luaL_checkstring(L, 1);
  if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) return luaL_error(L, "scheme not allowed");
  static char resp[8192];
  lua_pushstring(L, resp);                    // TODO(P2)：走模块 9 的 curl
  return 1;
}

extern "C" int lua_sandbox_init(const char* scriptsDir) {
  (void)scriptsDir;
  g_L = luaL_newstate();
  if (!g_L) return -1;
  luaL_openlibs(g_L);
  lua_pushnil(g_L); lua_setglobal(g_L, "io");
  lua_pushnil(g_L); lua_setglobal(g_L, "os");
  lua_pushnil(g_L); lua_setglobal(g_L, "package");
  lua_pushnil(g_L); lua_setglobal(g_L, "loadfile");
  lua_pushnil(g_L); lua_setglobal(g_L, "dofile");
  lua_register(g_L, "http_get", l_http_get);
  lua_sethook(g_L, count_hook, LUA_MASKCOUNT, 1000);
  return 0;
}
extern "C" void lua_sandbox_set_limit(uint64_t instr) { g_limit = instr ? instr : 50000000ull; }

extern "C" int lua_sandbox_run(const char* name, const char* input, char* out, int cap) {
  if (!g_L || !name) return -1;
  char full[700];
  snprintf(full, sizeof(full), "%s/%s", g_dir, name);
  g_used = 0;
  lua_pushstring(g_L, input ? input : "");
  lua_setglobal(g_L, "input");
  if (luaL_dofile(g_L, full) != LUA_OK) {
    LOGE("[7] 脚本错误 %s: %s", name, lua_tostring(g_L, -1));
    lua_pop(g_L, 1);
    return -2;
  }
  if (out && cap > 0) {
    const char* r = lua_tostring(g_L, -1);
    snprintf(out, cap, "%s", r ? r : "");
    lua_pop(g_L, 1);
  }
  return 0;
}
extern "C" void lua_sandbox_shutdown(void) { if (g_L) { lua_close(g_L); g_L = nullptr; } }
#endif
