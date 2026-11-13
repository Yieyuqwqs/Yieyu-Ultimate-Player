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

// ── 网络出口：调用 libyieyu_fetch.so（Go 写的 C ABI，自带 TLS）──────
// 没链接到时退化为返回空串，脚本会走"没结果"分支，不会崩。
#ifdef YIEYU_HAVE_FETCH
extern "C" {
char* yieyu_fetch(const char* url, const char* method, const char* body,
                  const char* headers, int* outLen);
void  yieyu_fetch_free(char* p);
}
#endif

static int l_http(lua_State* L) {
  const char* url = luaL_checkstring(L, 1);
  const char* body = lua_isnoneornil(L, 2) ? "" : luaL_checkstring(L, 2);
  if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) {
    return luaL_error(L, "scheme not allowed");
  }
  // http.post 这类调用允许第 3 个参数是 headers
  const char* headers = lua_isnoneornil(L, 3) ? "" : luaL_checkstring(L, 3);
  const bool isPost = !lua_isnoneornil(L, 2);

#ifdef YIEYU_HAVE_FETCH
  int outLen = 0;
  char* resp = yieyu_fetch(url, isPost ? "POST" : "GET", body, headers, &outLen);
  if (resp) {
    lua_pushlstring(L, resp, (size_t)outLen);
    yieyu_fetch_free(resp);
    return 1;
  }
  lua_pushstring(L, "");                    // 网络失败：给空串，脚本自行判断
  return 1;
#else
  (void)body; (void)headers; (void)isPost;
  lua_pushstring(L, "");
  return 1;
#endif
}
// 注意：LuaJIT 是 Lua 5.1，没有 luaL_tolstring（5.2+ 才有）。
// 这里用 lua_tostring 逐参数取字符串（数字会被就地转成字符串，5.1 行为）。
static int l_log(lua_State* L) {
  const int n = lua_gettop(L);
  for (int i = 1; i <= n; ++i) {
    const char* s = lua_tostring(L, i);
    if (s) bb_write(YIEYU_LOG_ERROR, "[7] script: %s", s);
  }
  return 0;
}

// ── 最小 JSON 编解码（不引第三方库）──────────────────────────────────
// 为什么必须有：落雪源脚本普遍用 json.decode 解源站响应，没有它所有现成脚本第一条就挂。
// 限制（刻意）：嵌套 ≤32、不做 NaN/Inf、unicode 转义非 ASCII 简化为 '?'。
// 兼容性：LuaJIT 是 Lua 5.1，禁用 lua_absindex / lua_rawlen（自己实现）。
namespace {

#if LUA_VERSION_NUM < 502
static int j_absidx(lua_State* L, int idx) {
  return (idx > 0 || idx <= LUA_REGISTRYINDEX) ? idx : lua_gettop(L) + idx + 1;
}
static size_t j_rawlen(lua_State* L, int idx) { return lua_objlen(L, idx); }
#else
static int j_absidx(lua_State* L, int idx) { return lua_absindex(L, idx); }
static size_t j_rawlen(lua_State* L, int idx) { return lua_rawlen(L, idx); }
#endif

struct JParse { const char* p; const char* end; lua_State* L; int depth; };

void jskip(JParse* s) {
  while (s->p < s->end && (*s->p == ' ' || *s->p == '\t' || *s->p == '\n' || *s->p == '\r')) ++s->p;
}

bool jvalue(JParse* s);

bool jstring(JParse* s) {
  if (s->p >= s->end || *s->p != '"') return false;
  ++s->p;
  char buf[4096];
  size_t n = 0;
  while (s->p < s->end) {
    char c = *s->p++;
    if (c == '"') { lua_pushlstring(s->L, buf, n); return true; }
    if (c == '\\' && s->p < s->end) {
      char e = *s->p++;
      switch (e) {
        case 'n': c = '\n'; break;
        case 't': c = '\t'; break;
        case 'r': c = '\r'; break;
        case 'b': c = '\b'; break;
        case 'f': c = '\f'; break;
        case 'u': {
          if (s->end - s->p < 4) return false;
          unsigned cp = 0;
          for (int i = 0; i < 4; ++i) {
            char h = *s->p++;
            cp <<= 4;
            if (h >= '0' && h <= '9') cp |= (unsigned)(h - '0');
            else if (h >= 'a' && h <= 'f') cp |= (unsigned)(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F') cp |= (unsigned)(h - 'A' + 10);
            else return false;
          }
          c = (cp < 0x80) ? (char)cp : '?';
          break;
        }
        default: c = e; break;
      }
    }
    if (n < sizeof(buf) - 1) buf[n++] = c;
  }
  return false;
}

bool jnumber(JParse* s) {
  char* endp = nullptr;
  double v = strtod(s->p, &endp);
  if (endp == s->p) return false;
  s->p = endp;
  lua_pushnumber(s->L, v);
  return true;
}

bool jarray(JParse* s) {
  ++s->p;
  lua_newtable(s->L);
  jskip(s);
  if (s->p < s->end && *s->p == ']') { ++s->p; return true; }
  int idx = 1;
  for (;;) {
    if (!jvalue(s)) return false;
    lua_rawseti(s->L, -2, idx++);
    jskip(s);
    if (s->p >= s->end) return false;
    if (*s->p == ',') { ++s->p; continue; }
    if (*s->p == ']') { ++s->p; return true; }
    return false;
  }
}

bool jobject(JParse* s) {
  ++s->p;
  lua_newtable(s->L);
  jskip(s);
  if (s->p < s->end && *s->p == '}') { ++s->p; return true; }
  for (;;) {
    jskip(s);
    if (!jstring(s)) return false;
    jskip(s);
    if (s->p >= s->end || *s->p != ':') return false;
    ++s->p;
    if (!jvalue(s)) return false;
    lua_settable(s->L, -3);
    jskip(s);
    if (s->p >= s->end) return false;
    if (*s->p == ',') { ++s->p; continue; }
    if (*s->p == '}') { ++s->p; return true; }
    return false;
  }
}

bool jvalue(JParse* s) {
  if (++s->depth > 32) return false;
  jskip(s);
  if (s->p >= s->end) { --s->depth; return false; }
  bool ok = false;
  switch (*s->p) {
    case '{': ok = jobject(s); break;
    case '[': ok = jarray(s); break;
    case '"': ok = jstring(s); break;
    case 't': if (s->end - s->p >= 4 && !strncmp(s->p, "true", 4)) { s->p += 4; lua_pushboolean(s->L, 1); ok = true; } break;
    case 'f': if (s->end - s->p >= 5 && !strncmp(s->p, "false", 5)) { s->p += 5; lua_pushboolean(s->L, 0); ok = true; } break;
    case 'n': if (s->end - s->p >= 4 && !strncmp(s->p, "null", 4)) { s->p += 4; lua_pushnil(s->L); ok = true; } break;
    default: ok = jnumber(s); break;
  }
  --s->depth;
  return ok;
}

int l_json_decode(lua_State* L) {
  size_t len = 0;
  const char* str = luaL_checklstring(L, 1, &len);
  JParse s{str, str + len, L, 0};
  if (!jvalue(&s)) return luaL_error(L, "json.decode: invalid input");
  return 1;
}

void jenc(lua_State* L, int idx, int depth);

void jenc_table(lua_State* L, int idx, int depth) {
  if (depth > 16) { lua_pushstring(L, "null"); return; }
  idx = j_absidx(L, idx);
  const size_t n = j_rawlen(L, idx);
  luaL_Buffer b;
  luaL_buffinit(L, &b);
  if (n > 0) {                                   // 数组
    luaL_addchar(&b, '[');
    for (size_t i = 1; i <= n; ++i) {
      if (i > 1) luaL_addchar(&b, ',');
      lua_rawgeti(L, idx, (int)i);
      jenc(L, -1, depth + 1);
      size_t l = 0;
      const char* sv = lua_tolstring(L, -1, &l);
      if (sv) luaL_addlstring(&b, sv, l);
      lua_pop(L, 2);
    }
    luaL_addchar(&b, ']');
  } else {                                        // 对象
    luaL_addchar(&b, '{');
    bool first = true;
    lua_pushnil(L);
    while (lua_next(L, idx) != 0) {
      if (!first) luaL_addchar(&b, ',');
      first = false;
      jenc(L, -2, depth + 1);
      size_t kl = 0;
      const char* ks = lua_tolstring(L, -1, &kl);
      if (ks) luaL_addlstring(&b, ks, kl);
      lua_pop(L, 1);
      luaL_addchar(&b, ':');
      jenc(L, -1, depth + 1);
      size_t vl = 0;
      const char* vs = lua_tolstring(L, -1, &vl);
      if (vs) luaL_addlstring(&b, vs, vl);
      lua_pop(L, 2);
    }
    luaL_addchar(&b, '}');
  }
  luaL_pushresult(&b);
}

void jenc_string(lua_State* L, const char* s, size_t n) {
  luaL_Buffer b;
  luaL_buffinit(L, &b);
  luaL_addchar(&b, '"');
  for (size_t i = 0; i < n; ++i) {
    char c = s[i];
    if (c == '"' || c == '\\') { luaL_addchar(&b, '\\'); luaL_addchar(&b, c); }
    else if (c == '\n') luaL_addstring(&b, "\\n");
    else if (c == '\r') luaL_addstring(&b, "\\r");
    else if (c == '\t') luaL_addstring(&b, "\\t");
    else luaL_addchar(&b, c);
  }
  luaL_addchar(&b, '"');
  luaL_pushresult(&b);
}

void jenc(lua_State* L, int idx, int depth) {
  idx = j_absidx(L, idx);
  switch (lua_type(L, idx)) {
    case LUA_TNIL: lua_pushstring(L, "null"); break;
    case LUA_TBOOLEAN: lua_pushstring(L, lua_toboolean(L, idx) ? "true" : "false"); break;
    case LUA_TNUMBER: { char tmp[64]; snprintf(tmp, sizeof(tmp), "%.10g", lua_tonumber(L, idx)); lua_pushstring(L, tmp); break; }
    case LUA_TSTRING: { size_t n = 0; const char* sv = lua_tolstring(L, idx, &n); jenc_string(L, sv, n); break; }
    default: jenc_table(L, idx, depth); break;
  }
}

int l_json_encode(lua_State* L) { jenc(L, 1, 0); return 1; }

}  // namespace

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

  // json 表（脚本一直在用，之前根本没有）
  lua_newtable(L);
  lua_pushcfunction(L, l_json_decode); lua_setfield(L, -2, "decode");
  lua_pushcfunction(L, l_json_encode); lua_setfield(L, -2, "encode");
  lua_setglobal(L, "json");
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
