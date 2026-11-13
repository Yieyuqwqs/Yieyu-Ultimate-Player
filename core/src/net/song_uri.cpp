// [9]/[5] 音源直链解析实现。见 song_uri.h 的约定。
#include "song_uri.h"

#include <cstdio>
#include <cstring>

#include "../common/blackbox.h"
#include "../common/log.h"

// 网络出口：Go 编的 libyieyu_fetch.so（同一份 .so 里也有 Lua 用的 yieyu_fetch_ex）
#ifdef YIEYU_HAVE_FETCH
extern "C" {
char* yieyu_music_url(const char* source, const char* id, const char* mid,
                      const char* quality, int* outLen);
void  yieyu_fetch_free(char* p);
}
#endif

namespace {

char g_err[192] = {0};

void set_err(const char* a, const char* b) {
  std::snprintf(g_err, sizeof(g_err), "%s%s", a ? a : "", b ? b : "");
  bb_write(YIEYU_LOG_ERROR, "[9] 直链解析失败：%s", g_err);
}

// 从 "k1=v1&k2=v2" 里取一个 key 的值（找到才写 out，找不到保持原样）
bool param(const char* q, const char* key, char* out, size_t cap) {
  if (!q || !key || !cap) return false;
  const size_t klen = std::strlen(key);
  const char* p = q;
  while (p && *p) {
    if (std::strncmp(p, key, klen) == 0 && p[klen] == '=') {
      const char* v = p + klen + 1;
      size_t n = 0;
      while (v[n] && v[n] != '&' && (n + 1) < cap) {
        out[n] = v[n];
        ++n;
      }
      out[n] = 0;
      return n > 0;
    }
    p = std::strchr(p, '&');
    if (p) ++p;
  }
  return false;
}

const char* kScheme = "yieyu://song";

}  // namespace

extern "C" const char* yieyu_song_last_error(void) { return g_err; }

extern "C" int yieyu_resolve_uri(const char* uri, char* out, size_t cap) {
  if (!uri || !*uri || !out || cap < 8) return -1;
  g_err[0] = 0;

  // 普通 URL / 本地路径：原样交给解码器（媒体框架两边都吃）
  if (std::strncmp(uri, kScheme, std::strlen(kScheme)) != 0) {
    std::snprintf(out, cap, "%s", uri);
    return 0;
  }

  const char* q = std::strchr(uri, '?');
  char source[32] = "wy";
  char id[96] = {0};
  char mid[96] = {0};
  char quality[24] = "master";
  if (q) {
    param(q + 1, "source", source, sizeof(source));
    param(q + 1, "id", id, sizeof(id));
    param(q + 1, "mid", mid, sizeof(mid));
    param(q + 1, "quality", quality, sizeof(quality));
  }
  if (!id[0] && !mid[0]) {
    set_err("内部 URI 里没有 id/mid：", uri);
    return -2;
  }

#ifdef YIEYU_HAVE_FETCH
  int n = 0;
  char* url = yieyu_music_url(source, id, mid, quality, &n);
  if (!url) {
    set_err("音源模块没返回（网络或接口失败）source=", source);
    return -3;
  }
  const size_t len = (n > 0) ? (size_t)n : std::strlen(url);
  int rc = 0;
  if (len == 0) {
    set_err("接口返回空直链（source=", source);
    rc = -4;
  } else if (len + 1 > cap) {
    set_err("直链超长，装不下：", nullptr);
    rc = -5;
  } else {
    std::memcpy(out, url, len);
    out[len] = 0;
  }
  yieyu_fetch_free(url);
  return rc;
#else
  set_err("本次构建没带 libyieyu_fetch.so（音源网络不可用）", nullptr);
  return -6;
#endif
}
