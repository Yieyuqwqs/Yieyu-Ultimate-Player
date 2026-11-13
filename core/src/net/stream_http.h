// [9] 最底层 TCP/Socket + HTTP/1.1 取流（自研，零第三方）。
//
// 边界（铁律 1）：这里只管"把字节从网上搬到本地"，**不解析 JSON**（那是 [6]），
// 不决定播什么（那是 [1]）。https 不在这里做：NDK 没有 TLS，https 全部交给
// 系统解码器直连（mediandk 自带网络栈）或 Go 的 libyieyu_fetch（模块 5 的产物）。
//
// 线程：会阻塞，只允许解码线程/工作线程调用。
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 下载一个 http:// URL 到 path。maxMB<=0 时默认 512MB。
// 返回 0=成功；<0=错误（见 yieyu_http_error）。内容长度进 outBytes（可传 NULL）。
int yieyu_http_download(const char* url, const char* path, int maxMB, int64_t* outBytes);

// 只发一个 HEAD/GET 探针，把状态码与关键头写进 outInfo（给诊断面板）。
// 返回 HTTP 状态码（>=100）；<0 = 网络/参数错误。
int yieyu_http_probe(const char* url, char* outInfo, int cap);

const char* yieyu_http_error(void);

#ifdef __cplusplus
}
#endif
