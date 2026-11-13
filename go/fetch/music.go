// 【模块 5 搜歌】真实音源接口（c.wwwweb.top）。
//
// 为什么放在 Go 而不是 C++：
//   ✗ C++ 侧要自己写 HTTPS + JSON 解析（上一版就是只有 TODO）。
//   ✓ Go 标准库自带 TLS1.3 + encoding/json，编进同一个 libyieyu_fetch.so 零额外依赖。
//
// 给 C++ 的契约（都在同一份 .so 里，与 yieyu_fetch_ex 共用进程）：
//   char* yieyu_music_search(keyword, page, limit, int* outLen)      → JSON 数组（失败给 "[]"）
//   char* yieyu_music_url(source, id, mid, quality, int* outLen)     → 直链字符串（失败给 ""）
//   返回的指针用 yieyu_fetch_free() 释放。
//
// 实测（2026-11-13 本机 curl 验过）：
//   POST /music/search {"keyword","page","limit"}           → {"code":200,"data":[{id,mid,name,artist,album}]}
//   POST /music/url    {"source","musicId","quality"}       → {"code":200,"url":"http://....flac"}
//   source 可用：wy / tx / kw（kg 常见"歌曲不存在"，mg 不支持）
//   ★ 这几个接口**不需要 API key**，所以仓库里不放任何密钥。
package main

/*
#include <stdlib.h>
*/
import "C"

import (
	"encoding/json"
	"io"
	"net/http"
	"strconv"
	"strings"
	"time"
)

const (
	musicBase     = "https://c.wwwweb.top"
	musicMaxBody  = 1024 * 1024
	musicDefaultQ = "master"
)

type musicSearchItem struct {
	ID     string `json:"id"`
	Mid    string `json:"mid"`
	Name   string `json:"name"`
	Artist string `json:"artist"`
	Album  string `json:"album"`
	Source string `json:"source"`
}

type musicSearchResp struct {
	Code int `json:"code"`
	Data []struct {
		ID     int64  `json:"id"`
		Mid    string `json:"mid"`
		Name   string `json:"name"`
		Artist string `json:"artist"`
		Album  string `json:"album"`
	} `json:"data"`
}

type musicURLResp struct {
	Code int    `json:"code"`
	URL  string `json:"url"`
}

var musicClient = &http.Client{
	Timeout: 25 * time.Second,
	Transport: &http.Transport{
		MaxIdleConns:        8,
		MaxIdleConnsPerHost: 4,
		IdleConnTimeout:     45 * time.Second,
		ForceAttemptHTTP2:   true,
	},
}

// musicPost：POST JSON → 解进 out。任何一步失败都返回 false（上层决定降级）。
func musicPost(path string, payload interface{}, out interface{}) bool {
	body, err := json.Marshal(payload)
	if err != nil {
		return false
	}
	req, err := http.NewRequest(http.MethodPost, musicBase+path, strings.NewReader(string(body)))
	if err != nil {
		return false
	}
	req.Header.Set("Content-Type", "application/json")
	req.Header.Set("Accept", "application/json")
	req.Header.Set("User-Agent", userAgent)
	resp, err := musicClient.Do(req)
	if err != nil {
		return false
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return false
	}
	buf, err := io.ReadAll(io.LimitReader(resp.Body, musicMaxBody))
	if err != nil {
		return false
	}
	return json.Unmarshal(buf, out) == nil
}

//export yieyu_music_search
func yieyu_music_search(ckw *C.char, page C.int, limit C.int, outLen *C.int) *C.char {
	if outLen != nil {
		*outLen = 0
	}
	kw := strings.TrimSpace(C.GoString(ckw))
	if kw == "" {
		return C.CString("[]")
	}
	p := int(page)
	if p < 1 {
		p = 1
	}
	n := int(limit)
	if n < 1 || n > 30 {
		n = 20
	}

	var r musicSearchResp
	if !musicPost("/music/search", map[string]interface{}{"keyword": kw, "page": p, "limit": n}, &r) {
		return C.CString("[]")
	}
	items := make([]musicSearchItem, 0, len(r.Data))
	for _, d := range r.Data {
		if d.Name == "" {
			continue
		}
		items = append(items, musicSearchItem{
			ID:     strconv.FormatInt(d.ID, 10),
			Mid:    d.Mid,
			Name:   d.Name,
			Artist: d.Artist,
			Album:  d.Album,
			Source: "wy",
		})
	}
	js, err := json.Marshal(items)
	if err != nil {
		return C.CString("[]")
	}
	s := string(js)
	if outLen != nil {
		*outLen = C.int(len(s))
	}
	return C.CString(s)
}

//export yieyu_music_url
func yieyu_music_url(csrc, cid, cmid, cq *C.char, outLen *C.int) *C.char {
	if outLen != nil {
		*outLen = 0
	}
	src := strings.TrimSpace(C.GoString(csrc))
	if src == "" {
		src = "wy"
	}
	q := strings.TrimSpace(C.GoString(cq))
	if q == "" {
		q = musicDefaultQ
	}
	// 先拿数字 id 试，不行再用 mid（不同源对这两个字段的偏好不一样）
	cands := make([]string, 0, 2)
	if v := strings.TrimSpace(C.GoString(cid)); v != "" {
		cands = append(cands, v)
	}
	if v := strings.TrimSpace(C.GoString(cmid)); v != "" {
		cands = append(cands, v)
	}
	for _, id := range cands {
		var r musicURLResp
		if !musicPost("/music/url", map[string]interface{}{"source": src, "musicId": id, "quality": q}, &r) {
			continue
		}
		if r.Code == 200 && strings.HasPrefix(r.URL, "http") {
			if outLen != nil {
				*outLen = C.int(len(r.URL))
			}
			return C.CString(r.URL)
		}
	}
	return C.CString("")
}

//export yieyu_music_version
func yieyu_music_version() C.int { return 1 }
