// Package main 【模块 9 的 HTTP 实现 / 模块 5 的共用出口】
//
// 为什么不在这里用 C 的 libcurl：
//   curl 要 HTTPS 就得连带 OpenSSL 或 mbedTLS 交叉编译（好几 MB + 一堆坑）。
//   Go 标准库自带 TLS 1.3 / HTTP2 / 连接池，`-buildmode=c-shared` 直接给 C ABI，
//   交叉编译零外部依赖 —— 这是这套架构里最省事也最稳的一条路。
//
// 给 Lua 沙箱（模块 7）的契约：
//   char* yieyu_fetch(url, method, body, headers, int* outLen)                    ← 旧
//   char* yieyu_fetch_ex(url, method, body, headers, int* outLen, int* outStatus)  ← 带状态码
//   void  yieyu_fetch_free(char* p)
//
// ★ 为什么要 ex：脚本判断成败靠的是 HTTP 状态码（`local body, code = http_post(...)`），
//   旧签名只回 body，脚本里 `if code ~= 200 then return nil end` 永远成立 → 永远拿不到 URL。
// 返回的是 C 堆内存（C.CString），调用方必须用 yieyu_fetch_free 释放。
package main

/*
#include <stdlib.h>
*/
import "C"

import (
	"io"
	"strings"
	"time"
	"unsafe"

	"net/http"
)

const (
	maxBody    = 256 * 1024 // 单次响应上限 256KB（脚本够了，防止被灌爆）
	userAgent  = "YieyuUltimate/0.1 (Android; +script-source)"
	reqTimeout = 12 * time.Second
)

var client = &http.Client{
	Timeout: reqTimeout,
	Transport: &http.Transport{
		MaxIdleConns:        8,
		MaxIdleConnsPerHost: 2,
		IdleConnTimeout:     30 * time.Second,
		ForceAttemptHTTP2:   true,
	},
}

// doFetch 是唯一实现：body + HTTP 状态码（网络层失败时 status = 0）
func doFetch(curl *C.char, cmethod *C.char, cbody *C.char, cheaders *C.char, outLen *C.int, outStatus *C.int) *C.char {
	if outLen != nil {
		*outLen = 0
	}
	if outStatus != nil {
		*outStatus = 0
	}
	url := C.GoString(curl)
	if url == "" {
		return nil
	}
	if !strings.HasPrefix(url, "http://") && !strings.HasPrefix(url, "https://") {
		return nil // 只允许 http(s)，与沙箱白名单一致
	}
	method := strings.ToUpper(C.GoString(cmethod))
	if method == "" {
		method = http.MethodGet
	}
	var bodyReader io.Reader
	if b := C.GoString(cbody); b != "" {
		bodyReader = strings.NewReader(b)
	}

	req, err := http.NewRequest(method, url, bodyReader)
	if err != nil {
		return nil
	}
	req.Header.Set("User-Agent", userAgent)
	req.Header.Set("Accept", "*/*")
	// 附加头：每行 "K: V"
	if h := C.GoString(cheaders); h != "" {
		for _, line := range strings.Split(h, "\n") {
			if i := strings.Index(line, ":"); i > 0 {
				req.Header.Set(strings.TrimSpace(line[:i]), strings.TrimSpace(line[i+1:]))
			}
		}
	}

	resp, err := client.Do(req)
	if err != nil {
		return nil
	}
	defer resp.Body.Close()
	if outStatus != nil {
		*outStatus = C.int(resp.StatusCode)
	}

	buf, err := io.ReadAll(io.LimitReader(resp.Body, maxBody))
	if err != nil {
		return nil
	}
	if outLen != nil {
		*outLen = C.int(len(buf))
	}
	// 注意：C.CString 会复制并在末尾补 \0，正好给 C++ 侧当字符串用
	return C.CString(string(buf))
}

//export yieyu_fetch
func yieyu_fetch(curl *C.char, cmethod *C.char, cbody *C.char, cheaders *C.char, outLen *C.int) *C.char {
	return doFetch(curl, cmethod, cbody, cheaders, outLen, nil)
}

//export yieyu_fetch_ex
func yieyu_fetch_ex(curl *C.char, cmethod *C.char, cbody *C.char, cheaders *C.char, outLen *C.int, outStatus *C.int) *C.char {
	return doFetch(curl, cmethod, cbody, cheaders, outLen, outStatus)
}

//export yieyu_fetch_free
func yieyu_fetch_free(p *C.char) {
	C.free(unsafe.Pointer(p))
}

//export yieyu_fetch_version
func yieyu_fetch_version() C.int { return 1 }

func main() {}
