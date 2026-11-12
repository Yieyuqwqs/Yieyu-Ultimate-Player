// [9] libcurl：只管最底层的 TCP/Socket 与流式拉取，绝不解析 JSON（那是 [6] TS 的活）。
// 给 [13] FFmpeg 提供 AVIO 回调：读/seek 都在这里。
#include <stddef.h>
#include <stdint.h>
#include "../common/log.h"

#ifdef YIEYU_OFFLINE
int  yieyu_curl_open(const char* url, int64_t rangeStart) { (void)url; (void)rangeStart; LOGW("[9]curl 空实现"); return 0; }
long yieyu_curl_read(void* h, void* buf, long n) { (void)h; (void)buf; (void)n; return 0; }
long long yieyu_curl_seek(void* h, long long off, int whence) { (void)h; (void)off; (void)whence; return 0; }
void yieyu_curl_close(void* h) { (void)h; }
#else
#include <curl/curl.h>
typedef struct { void* easy; int64_t pos, size; } yieyu_stream;

static size_t write_cb(char* p, size_t sz, size_t n, void* ud) {
  /* 直接写入模块 13 的字节环（在 handler.h 里暴露的 push 接口）*/
  return yieyu_bytering_push(ud, p, sz * n);
}

int yieyu_curl_open(const char* url, int64_t rangeStart) {
  static int inited = 0;
  if (!inited) { curl_global_init(CURL_GLOBAL_DEFAULT); inited = 1; }
  CURL* e = curl_easy_init();
  if (!e) return -1;
  curl_easy_setopt(e, CURLOPT_URL, url);
  curl_easy_setopt(e, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(e, CURLOPT_RANGE, rangeStart > 0 ? "0-" : NULL);   /* TODO(P2)：精确 Range */
  curl_easy_setopt(e, CURLOPT_USERAGENT, "YieyuUltimate/0.1");
  curl_easy_setopt(e, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2TLS);
  curl_easy_setopt(e, CURLOPT_TCP_KEEPALIVE, 1L);
  LOGI("[9] 开流 %s", url);
  return 0;
}
long yieyu_curl_read(void* h, void* buf, long n) { (void)h; (void)buf; (void)n; return 0; }
long long yieyu_curl_seek(void* h, long long off, int whence) { (void)h; (void)off; (void)whence; return -1; }
void yieyu_curl_close(void* h) { (void)h; }
#endif
