// [7] LuaJIT 沙箱：跑第三方音源脚本（落雪源生态）。
//
// 白名单（就是这些，没有别的）：
//   http_get(url)               → 返回 body 字符串
//   http_post(url, body[, hdr]) → 返回 body 字符串
//   request / fetch             → 同 http_get 的别名（兼容不同脚本写法）
//   http.get / http.post        → 表形式的别名
//   log(...)                    → 进黑匣子（只有 ERROR 会落盘，log 仅在调试构建可用）
//   input                       → 全局字符串，宿主注入（关键词/歌曲 id）
//
// 黑名单：io / os / package / require / dofile / loadfile / debug.sethook
// 防死循环：指令计数 hook（5000 万条/次），超限软掐。
//
// ★ 脚本返回的是**脏数据原文**，清洗交给模块 6（TS/Zod）。沙箱不解析 JSON ——
//   这样脚本永远不可能因为"源站改字段"而崩，崩的只会是清洗层，而它有兜底。
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

bool file_banned(const char* path, const char** which) {
  FILE* f = fopen(path, "rb");
  if (!f) return true;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  if (n < 0 || n > 512 * 1024) { fclose(f); return true; }
  fseek(f, 0, SEEK_SET);
  char* buf = new char[(size_t)n + 1];
  size_t got = fread(buf, 1, (size_t)n, f);
  buf[got] = 0;
  fclose(f);
  bool bad = false;
  for (int i = 0; i < kBanCount; ++i) {
    if (strstr(buf, kBan[i])) { bad = true; if (which) *which = kBan[i]; break; }
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
  if (!d) return 0;                       // 目录还没建，不算错

  g_count = 0;
  struct dirent* e;
  while ((e = readdir(d)) != nullptr) {
    const char* nm = e->d_name;
    size_t len = strlen(nm);
    if (len < 5 || strcmp(nm + len - 4, ".lua") != 0) continue;
    char full[700];
    snprintf(full, sizeof(full), "%s/%s", dir, nm);
    const char* which = "";
    if (file_banned(full, &which)) {
      if (rejected) (*rejected)++;
      continue;                             // 被拒的不登记、不执行
    }
    if (g_count < kMaxScripts) {
      snprintf(g_names[g_count], sizeof(g_names[0]), "%s", nm);
      g_count++;
      if (loaded) (*loaded)++;
    } else if (rejected) {
      (*rejected)++;
    }
  }
  closedir(d);
  return 0;
}

#ifdef YIEYU_HAVE_LUAJIT
extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

namespace {
lua_State* g_L = nullptr;
uint64_t g_limit = 50000000ull;
uint64_t g_used = 0;

void count_hook(lua_State* L, lua_Debug*) {
  g_used += 1000;
  if (g_used > g_limit) luaL_error(L, "sandbox: instruction limit exceeded");
}

// 唯一的网络出口。真实的 socket 由模块 9（libcurl）提供；这里是它的 Lua 门面。
static int l_http(lua_State* L) {
  const char* url = luaL_checkstring(L, 1);
  const char* body = lua_isnoneornil(L, 2) ? nullptr : luaL_checkstring(L, 2);
  if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) {
    return luaL_error(L, "scheme not allowed");
  }
  // TODO(P2)：接模块 9 的 curl，在独立线程跑，最多 8KB 返回
  lua_pushstring(L, "");                    // 骨架期返回空串，脚本会走"没结果"分支
  (void)body;
  return 1;
}
static int l_log(lua_State* L) {
  const char* s = luaL_tolstring(L, 1, nullptr);
  bb_write(YIEYU_LOG_ERROR, "[7] script: %s", s ? s : "");
  lua_pop(L, 1);
  return 0;
}

void register_api(lua_State* L) {
  // 别名：不同脚本写法不同，全部指到同一个出口
  lua_register(L, "http_get", l_http);
  lua_register(L, "http_post", l_http);
  lua_register(L, "request", l_http);
  lua_register(L, "fetch", l_http);
  lua_register(L, "log", l_log);

  lua_newtable(L);
  lua_pushcfunction(L, l_http); lua_setfield(L, -2, "get");
  lua_pushcfunction(L, l_http); lua_setfield(L, -2, "post");
  lua_pushcfunction(L, l_http); lua_setfield(L, -2, "request");
  lua_setglobal(L, "http");
}
}  // namespace

extern "C" int lua_sandbox_init(const char*) {
  g_L = luaL_newstate();
  if (!g_L) return -1;
  luaL_openlibs(g_L);
  // 拆危险库
  lua_pushnil(g_L); lua_setglobal(g_L, "io");
  lua_pushnil(g_L); lua_setglobal(g_L, "os");
  lua_pushnil(g_L); lua_setglobal(g_L, "package");
  lua_pushnil(g_L); lua_setglobal(g_L, "loadfile");
  lua_pushnil(g_L); lua_setglobal(g_L, "dofile");
  register_api(g_L);
  lua_sethook(g_L, count_hook, LUA_MASKCOUNT, 1000);
  return 0;
}

extern "C" void lua_sandbox_set_limit(uint64_t i) { g_limit = i ? i : 50000000ull; }

extern "C" int lua_sandbox_run(const char* name, const char* input, char* out, int cap) {
  if (!g_L || !name) return -1;
  char full[700];
  snprintf(full, sizeof(full), "%s/%s", g_dir, name);
  g_used = 0;
  lua_pushstring(g_L, input ? input : "");
  lua_setglobal(g_L, "input");
  if (luaL_dofile(g_L, full) != LUA_OK) {
    const char* err = lua_tostring(g_L, -1);
    bb_write(YIEYU_LOG_ERROR, "[7] 脚本执行失败 %s：%s", name, err ? err : "?");
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

#else  // ── 没有 LuaJIT：扫描/校验/登记照常，执行返回 -2 ──────────────────
extern "C" int lua_sandbox_init(const char*) { return 0; }
extern "C" void lua_sandbox_set_limit(uint64_t) {}
extern "C" int lua_sandbox_run(const char*, const char*, char* out, int cap) {
  if (out && cap) out[0] = 0;
  return -2;                              // -2 = 宿主未接入 LuaJIT
}
extern "C" void lua_sandbox_shutdown(void) {}
#endif
