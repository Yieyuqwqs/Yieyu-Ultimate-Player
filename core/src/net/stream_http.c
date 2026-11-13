// [9] 自研 HTTP/1.1 客户端（阻塞式，单次请求）：DNS → TCP → 请求 → 头解析 → 正文落盘。
// 支持：重定向（≤5 跳）、chunked 分块、Content-Length、连接超时、大小上限。
// 不支持（明确报错，不假装成功）：https（NDK 无 TLS）、HTTP 代理、断点续传。
#include "stream_http.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "../common/log.h"

#define YIEYU_HTTP_MAX_REDIRECT 5
#define YIEYU_HTTP_HDR_MAX 16384
#define YIEYU_HTTP_TIMEOUT_MS 15000

static char g_err[200] = {0};

static void set_err(const char* fmt, const char* a, int b) {
  snprintf(g_err, sizeof(g_err), fmt, a ? a : "", b);
}

const char* yieyu_http_error(void) { return g_err; }

typedef struct {
  char host[256];
  int  port;
  char path[1024];
} Url;

static int parse_url(const char* url, Url* u) {
  if (!url) return -1;
  if (strncmp(url, "https://", 8) == 0) { set_err("https 需要 TLS（NDK 无），不在这里做%s%d", "", 0); return -2; }
  const char* p = url;
  if (strncmp(p, "http://", 7) == 0) p += 7;
  else if (strstr(p, "://")) return -3;          // 其它协议不管
  const char* slash = strchr(p, '/');
  const char* hostEnd = slash ? slash : (p + strlen(p));
  const char* colon = NULL;
  for (const char* q = p; q < hostEnd; ++q) if (*q == ':') colon = q;
  size_t hostLen = colon ? (size_t)(colon - p) : (size_t)(hostEnd - p);
  if (hostLen == 0 || hostLen >= sizeof(u->host)) return -4;
  memcpy(u->host, p, hostLen);
  u->host[hostLen] = 0;
  u->port = 80;
  if (colon) {
    int port = atoi(colon + 1);
    if (port <= 0 || port > 65535) return -5;
    u->port = port;
  }
  snprintf(u->path, sizeof(u->path), "%s", (slash && *slash) ? slash : "/");
  return 0;
}

static int wait_fd(int fd, int writeMode, int timeoutMs) {
  struct pollfd pfd;
  pfd.fd = fd;
  pfd.events = writeMode ? POLLOUT : POLLIN;
  pfd.revents = 0;
  for (;;) {
    const int rc = poll(&pfd, 1, timeoutMs);
    if (rc > 0) return 0;
    if (rc == 0) return -1;
    if (errno == EINTR) continue;
    return -2;
  }
}

static int tcp_connect(const char* host, int port) {
  char portStr[8];
  snprintf(portStr, sizeof(portStr), "%d", port);
  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  struct addrinfo* res = NULL;
  if (getaddrinfo(host, portStr, &hints, &res) != 0 || !res) {
    set_err("DNS 解析失败 %s%d", host, 0);
    return -1;
  }
  int fd = -1;
  for (struct addrinfo* ai = res; ai; ai = ai->ai_next) {
    const int s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (s < 0) continue;
    const int flags = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, flags | O_NONBLOCK);
    int rc = connect(s, ai->ai_addr, ai->ai_addrlen);
    if (rc != 0 && errno == EINPROGRESS) {
      if (wait_fd(s, 1, YIEYU_HTTP_TIMEOUT_MS) == 0) {
        int soErr = 0;
        socklen_t len = sizeof(soErr);
        if (getsockopt(s, SOL_SOCKET, SO_ERROR, &soErr, &len) == 0 && soErr == 0) rc = 0;
        else rc = -1;
      } else {
        rc = -1;
      }
    }
    if (rc == 0) { fd = s; break; }
    close(s);
  }
  freeaddrinfo(res);
  if (fd < 0) set_err("连不上 %s%d", host, 0);
  return fd;
}

static int send_all(int fd, const char* buf, size_t n) {
  size_t sent = 0;
  while (sent < n) {
    const ssize_t w = send(fd, buf + sent, n - sent, 0);
    if (w < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) { if (wait_fd(fd, 1, YIEYU_HTTP_TIMEOUT_MS) != 0) return -1; continue; }
      if (errno == EINTR) continue;
      return -2;
    }
    if (w == 0) return -3;
    sent += (size_t)w;
  }
  return 0;
}

// ── 一次请求：返回 0 成功（正文写进 FILE*），状态码进 outStatus ─────────
static int one_request(const Url* u, FILE* out, int64_t* written, int maxBytes, int* outStatus,
                       char* redirectOut, size_t redirectCap) {
  const int fd = tcp_connect(u->host, u->port);
  if (fd < 0) return -1;

  char req[1400];
  snprintf(req, sizeof(req),
           "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: YieyuUltimate/0.1\r\n"
           "Accept: */*\r\nConnection: close\r\n\r\n",
           u->path, u->host);
  if (send_all(fd, req, strlen(req)) != 0) { close(fd); set_err("发请求失败 %s%d", u->host, 0); return -2; }

  // 读头：一直读到 \r\n\r\n
  char hdr[YIEYU_HTTP_HDR_MAX];
  size_t hlen = 0;
  while (hlen < sizeof(hdr) - 1) {
    if (wait_fd(fd, 0, YIEYU_HTTP_TIMEOUT_MS) != 0) { close(fd); set_err("等响应超时 %s%d", u->host, 0); return -3; }
    const ssize_t r = recv(fd, hdr + hlen, sizeof(hdr) - 1 - hlen, 0);
    if (r <= 0) { close(fd); set_err("响应头读不到 %s%d", u->host, 0); return -4; }
    hlen += (size_t)r;
    hdr[hlen] = 0;
    if (strstr(hdr, "\r\n\r\n")) break;
  }
  char* hdrEnd = strstr(hdr, "\r\n\r\n");
  if (!hdrEnd) { close(fd); set_err("响应头不完整 %s%d", u->host, 0); return -5; }
  *hdrEnd = 0;
  const size_t headLen = (size_t)((hdrEnd + 4) - hdr);

  int status = 0;
  {
    const char* sp = strchr(hdr, ' ');
    if (!sp) { close(fd); set_err("状态行非法 %s%d", u->host, 0); return -6; }
    status = atoi(sp + 1);
  }
  if (outStatus) *outStatus = status;

  // 小写化一份头，方便找关键字段
  char lower[YIEYU_HTTP_HDR_MAX];
  for (size_t i = 0; i < hlen && i < sizeof(lower) - 1; ++i) {
    const char c = hdr[i];
    lower[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
  }
  lower[hlen < sizeof(lower) ? hlen : sizeof(lower) - 1] = 0;

  const int chunked = strstr(lower, "transfer-encoding: chunked") != NULL;
  int64_t clen = -1;
  {
    const char* p = strstr(lower, "content-length:");
    if (p) clen = atoll(p + 15);
  }

  // 重定向：把 Location 原样带出去，由调用方决定跳到哪（绝对地址或站内相对路径）
  if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
    if (redirectOut && redirectCap > 0) {
      redirectOut[0] = 0;
      const char* p = strstr(lower, "location:");
      if (p) {
        const char* v = p + 9;
        while (*v == ' ') ++v;
        // 在原始头里取同位置的原文（lower 只是小写版，长度一致）
        const size_t off = (size_t)(v - lower);
        size_t n = 0;
        while (off + n < hlen && hdr[off + n] != '\r' && hdr[off + n] != '\n' && n + 1 < redirectCap) {
          redirectOut[n] = hdr[off + n];
          ++n;
        }
        redirectOut[n] = 0;
      }
    }
    close(fd);
    return 1;
  }
  if (status < 200 || status >= 300) { close(fd); return -7; }

  // 正文：头部之后可能已经有部分正文字节在 hdr[headLen..hlen]
  int64_t total = 0;
  int rc = 0;

  if (chunked) {
    // 极简 chunked：把已收到的部分 + 后续字节当作流，按块长取
    char buf[8192];
    size_t blen = hlen - headLen;
    if (blen) memcpy(buf, hdr + headLen, blen);
    int eof = 0;
    for (;;) {
      // 需要一行：块长
      char line[32];
      size_t ll = 0;
      while (ll < sizeof(line) - 1) {
        if (blen == 0) {
          if (eof) { rc = -8; break; }
          if (wait_fd(fd, 0, YIEYU_HTTP_TIMEOUT_MS) != 0) { rc = -9; break; }
          const ssize_t r = recv(fd, buf, sizeof(buf), 0);
          if (r <= 0) { eof = 1; break; }
          blen = (size_t)r;
        }
        const char c = buf[0];
        memmove(buf, buf + 1, blen - 1);
        blen--;
        if (c == '\n') break;
        if (c != '\r') line[ll++] = c;
      }
      line[ll] = 0;
      if (ll == 0) { rc = -10; break; }
      const long size = strtol(line, NULL, 16);
      if (size <= 0) break;                       // 0 = 结束块
      long left = size;
      while (left > 0) {
        if (blen == 0) {
          if (wait_fd(fd, 0, YIEYU_HTTP_TIMEOUT_MS) != 0) { rc = -11; break; }
          const ssize_t r = recv(fd, buf, sizeof(buf), 0);
          if (r <= 0) { rc = -12; break; }
          blen = (size_t)r;
        }
        size_t take = (size_t)left < blen ? (size_t)left : blen;
        if (maxBytes > 0 && total + (int64_t)take > maxBytes) take = (size_t)(maxBytes - total);
        if (take) {
          if (fwrite(buf, 1, take, out) != take) { rc = -13; break; }
          total += (int64_t)take;
        }
        memmove(buf, buf + take, blen - take);
        blen -= take;
        left -= (long)take;
        if (maxBytes > 0 && total >= maxBytes) { rc = -14; break; }
      }
      if (rc != 0) break;
      // 吃掉块尾 \r\n
      int need = 2;
      while (need > 0) {
        if (blen == 0) {
          if (wait_fd(fd, 0, YIEYU_HTTP_TIMEOUT_MS) != 0) break;
          const ssize_t r = recv(fd, buf, sizeof(buf), 0);
          if (r <= 0) break;
          blen = (size_t)r;
        }
        memmove(buf, buf + 1, blen - 1);
        blen--;
        need--;
      }
      if (maxBytes > 0 && total >= maxBytes) { rc = -14; break; }
    }
  } else {
    if (clen > 0 && maxBytes > 0 && clen > maxBytes) clen = maxBytes;
    // 已收到的正文
    size_t blen = hlen - headLen;
    if (blen) {
      size_t take = blen;
      if (clen > 0 && (int64_t)take > clen) take = (size_t)clen;
      if (maxBytes > 0 && total + (int64_t)take > maxBytes) take = (size_t)(maxBytes - total);
      if (take && fwrite(hdr + headLen, 1, take, out) != take) rc = -13;
      else total += (int64_t)take;
    }
    char buf[16384];
    while (rc == 0 && (clen < 0 || total < clen)) {
      if (wait_fd(fd, 0, YIEYU_HTTP_TIMEOUT_MS) != 0) { rc = -9; break; }
      size_t want = sizeof(buf);
      if (clen > 0 && (int64_t)want > (clen - total)) want = (size_t)(clen - total);
      if (maxBytes > 0 && (int64_t)want > (maxBytes - total)) want = (size_t)(maxBytes - total);
      if (want == 0) break;
      const ssize_t r = recv(fd, buf, want, 0);
      if (r < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
        if (errno == EINTR) continue;
        rc = -15;
        break;
      }
      if (r == 0) break;                         // 对端关闭 = 结束
      if (fwrite(buf, 1, (size_t)r, out) != (size_t)r) { rc = -13; break; }
      total += (int64_t)r;
      if (maxBytes > 0 && total >= maxBytes) { rc = -14; break; }
    }
  }

  close(fd);
  if (written) *written = total;
  if (rc != 0 && rc != -14) { set_err("下载中断 errno%s%d", "", rc); return rc; }
  return 0;
}

int yieyu_http_download(const char* url, const char* path, int maxMB, int64_t* outBytes) {
  g_err[0] = 0;
  if (!url || !path) return -1;
  if (outBytes) *outBytes = 0;
  const int maxBytes = (maxMB > 0 ? maxMB : 512) * 1024 * 1024;

  char cur[1200];
  snprintf(cur, sizeof(cur), "%s", url);
  for (int hop = 0; hop <= YIEYU_HTTP_MAX_REDIRECT; ++hop) {
    Url u;
    memset(&u, 0, sizeof(u));
    const int pr = parse_url(cur, &u);
    if (pr != 0) return pr;

    FILE* f = fopen(path, "wb");
    if (!f) { set_err("写不了文件 %s%d", path, 0); return -20; }
    int64_t written = 0;
    int status = 0;
    char loc[1024];
    loc[0] = 0;
    const int rc = one_request(&u, f, &written, maxBytes, &status, loc, sizeof(loc));
    fflush(f);
    fclose(f);
    if (rc == 1) {
      if (!loc[0]) { set_err("重定向没有 Location%s%d", "", status); return -21; }
      if (strncmp(loc, "http://", 7) == 0 || strncmp(loc, "https://", 8) == 0) {
        snprintf(cur, sizeof(cur), "%s", loc);
      } else if (loc[0] == '/') {
        snprintf(cur, sizeof(cur), "http://%s:%d%s", u.host, u.port, loc);
      } else {
        // 相对路径（极少见）：拼到当前目录后面
        char dir[1024];
        snprintf(dir, sizeof(dir), "%s", u.path);
        char* sl = strrchr(dir, '/');
        if (sl) *(sl + 1) = 0;
        snprintf(cur, sizeof(cur), "http://%s:%d%s%s", u.host, u.port, dir, loc);
      }
      continue;                                  // 下一跳
    }
    if (rc != 0) return rc;
    if (outBytes) *outBytes = written;
    return 0;
  }
  set_err("重定向超过 %d 次%s", "", YIEYU_HTTP_MAX_REDIRECT);
  return -22;
}

int yieyu_http_probe(const char* url, char* outInfo, int cap) {
  g_err[0] = 0;
  Url u;
  memset(&u, 0, sizeof(u));
  const int pr = parse_url(url ? url : "", &u);
  if (pr != 0) return pr;
  FILE* f = tmpfile();
  if (!f) return -20;
  int64_t written = 0;
  int status = 0;
  char loc[1024];
  loc[0] = 0;
  const int rc = one_request(&u, f, &written, 64 * 1024, &status, loc, sizeof(loc));
  fclose(f);
  if (rc != 0 && rc != 1) return rc;
  if (outInfo && cap > 0) snprintf(outInfo, (size_t)cap, "status=%d bytes=%lld host=%s%s%s",
                                   status, (long long)written, u.host, loc[0] ? " -> " : "", loc);
  return status > 0 ? status : -1;
}
