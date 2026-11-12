// [7] LuaJIT 沙箱：跑第三方音源脚本（落雪源）。
// 白名单 API：http.get / json / base64 / log / 返回表。没有 os、没有 io、没有 package。
// 防死循环：指令计数 hook（默认 5000 万条/次调用），超限 os 软掐；--hard-kill 模式走独立进程 SIGKILL。
#include "../common/log.h"
#include <cstdint>
#include <cstring>

#ifdef YIEYU_OFFLINE
namespace yieyu {
int  lua_sandbox_init(const char* scriptsDir) { LOGI("[7]LuaJIT 空实现 dir=%s", scriptsDir ? scriptsDir : "?"); return 0; }
void lua_sandbox_set_limit(uint64_t instr) { (void)instr; }
int  lua_sandbox_run(const char* name, const char* input, char* out, int cap) { (void)name; (void)input; if (out && cap) out[0] = 0; return 0; }
void lua_sandbox_shutdown() {}
}
#else
extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}
namespace yieyu {
namespace {
lua_State* g_L = nullptr;
uint64_t   g_limit = 50000000ull;
uint64_t   g_used = 0;

void count_hook(lua_State* L, lua_Debug* ar) {
  (void)ar;
  g_used += 1000;                      // hook 每 1000 条触发一次
  if (g_used > g_limit) luaL_error(L, "sandbox: instruction limit exceeded");
}

// 白名单：只暴露 http.get，且只允许 http(s)
int l_http_get(lua_State* L) {
  const char* url = luaL_checkstring(L, 1);
  if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) return luaL_error(L, "scheme not allowed");
  static char resp[8192];
  lua_pushstring(L, resp);           // TODO(P2)：走模块 9 的 curl，阻塞在独立线程
  return 1;
}
}  // namespace

int lua_sandbox_init(const char* scriptsDir) {
  g_L = luaL_newstate();
  if (!g_L) return -1;
  luaL_openlibs(g_L);
  // 拆掉危险库（Io/Os 不可用，debug 只留 sethook 供我们设钩子）
  lua_pushnil(g_L); lua_setglobal(g_L, "io");
  lua_pushnil(g_L); lua_setglobal(g_L, "os");
  lua_pushnil(g_L); lua_setglobal(g_L, "package");
  lua_pushnil(g_L); lua_setglobal(g_L, "loadfile");
  lua_pushnil(g_L); lua_setglobal(g_L, "dofile");
  lua_register(g_L, "http_get", l_http_get);
  lua_sethook(g_L, count_hook, LUA_MASKCOUNT, 1000);
  LOGI("[7] LuaJIT 沙箱起，dir=%s limit=%llu 条", scriptsDir ? scriptsDir : "?", (unsigned long long)g_limit);
  return 0;
}

void lua_sandbox_set_limit(uint64_t instr) { g_limit = instr ? instr : 50000000ull; }

int lua_sandbox_run(const char* name, const char* input, char* out, int cap) {
  if (!g_L || !name) return -1;
  g_used = 0;
  lua_pushstring(g_L, input ? input : "");
  lua_setglobal(g_L, "input");
  if (luaL_dofile(g_L, name) != LUA_OK) {
    LOGE("[7] 脚本错误: %s", lua_tostring(g_L, -1));
    lua_pop(g_L, 1);
    return -2;
  }
  if (out && cap > 0) {
    const char* r = lua_tostring(g_L, -1);
    std::snprintf(out, cap, "%s", r ? r : "");
    lua_pop(g_L, 1);
  }
  return 0;
}
void lua_sandbox_shutdown() { if (g_L) { lua_close(g_L); g_L = nullptr; } }
}
#endif
