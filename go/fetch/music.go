// 【模块 5 搜歌 / 取直链】音源适配层（双通道）。
//
// 两个后端，都用：
//   ① c.wwwweb.top（老板给的 key：X-Api-Key）—— 搜索好使、数据全；
//      但 2026-11-14 实测 /music/url 返回 code=500（拿不到直链）
//   ② music-api.gdstudio.xyz（公开聚合 API，无需 key）—— search 与 url 实测都能通，
//      而且它 search 出来的 id 正好能被它自己的 url 接口解析
// 策略：**搜索优先 ①（老板的 key 不能白买），取直链优先 ②**（只有它能真出声音）；
//       任一失败自动落到另一个，绝不让 UI 卡住（每个请求 8 秒硬超时）。
//
// 契约（libyieyu_fetch.so 的 C ABI）：
//   char* yieyu_music_search(keyword, page, limit, int* outLen)  → JSON 数组（失败 "[]"）
//   char* yieyu_music_url(source, id, mid, quality, int* outLen) → 直链（失败 ""）
//   返回的指针用 yieyu_fetch_free() 释放。
package main

/*
#include <stdlib.h>
*/
import "C"

import (
	"encoding/json"
	"io"
	"net/http"
	"net/url"
	"strconv"
	"strings"
	"time"
)

const (
	cwBase    = "https://c.wwwweb.top"
	cwKey     = "IKM-P41000001-wqWCHmI24oNppwlH-OE" // 老板给的卡密式 key（搜索通道）
	gdBase    = "https://music-api.gdstudio.xyz/api.php"
	musicMaxBody = 1024 * 1024
	musicTimeout = 8 * time.Second
)

var musicClient = &http.Client{
	Timeout: musicTimeout,
	Transport: &http.Transport{
		MaxIdleConns:        8,
		MaxIdleConnsPerHost: 4,
		IdleConnTimeout:     45 * time.Second,
	},
}

// ── 统一的结果结构 ────────────────────────────────────────────────
type songItem struct {
	ID     string `json:"id"`
	Mid    string `json:"mid"`
	Name   string `json:"name"`
	Artist string `json:"artist"`
	Album  string `json:"album"`
	Source string `json:"source"`
}

func gdSource(src string) string {
	switch strings.ToLower(strings.TrimSpace(src)) {
	case "", "wy", "netease":
		return "netease"
	case "tx", "qq", "tencent":
		return "tencent"
	case "kw", "kugou", "kg":
		return "kugou"
	case "mg", "migu":
		return "migu"
	default:
		return "netease"
	}
}

// ── 通道 ①：c.wwwweb.top（POST JSON + X-Api-Key）──────────────────
func cwPost(path string, payload interface{}, out interface{}) bool {
	body, err := json.Marshal(payload)
	if err != nil {
		return false
	}
	req, err := http.NewRequest(http.MethodPost, cwBase+path, strings.NewReader(string(body)))
	if err != nil {
		return false
	}
	req.Header.Set("Content-Type", "application/json")
	req.Header.Set("Accept", "application/json")
	req.Header.Set("X-Api-Key", cwKey)
	req.Header.Set("User-Agent", "YieyuUltimate/0.2 (Android)")
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

// ── 通道 ②：gdstudio（GET 查询串）────────────────────────────────
func gdGet(params url.Values, out interface{}) bool {
	params.Set("_t", strconv.FormatInt(time.Now().UnixMilli(), 10))
	req, err := http.NewRequest(http.MethodGet, gdBase+"?"+params.Encode(), nil)
	if err != nil {
		return false
	}
	req.Header.Set("Accept", "application/json")
	req.Header.Set("User-Agent", "Mozilla/5.0 (Linux; Android 13) YieyuUltimate/0.2")
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

// 搜索：gdstudio 优先（它的 id 能被它自己的 url 接口解析 → 才真出声音），
// c.wwwweb.top（老板的 key）作为备用。
func searchGd(kw string, p, n int) []songItem {
	params := url.Values{}
	params.Set("types", "search")
	params.Set("source", "netease")
	params.Set("name", kw)
	params.Set("count", strconv.Itoa(n))
	params.Set("pages", strconv.Itoa(p))
	var items []struct {
		ID     string   `json:"id"`
		Name   string   `json:"name"`
		Artist []string `json:"artist"`
		Album  string   `json:"album"`
	}
	out := make([]songItem, 0, n)
	if !gdGet(params, &items) {
		return out
	}
	for _, it := range items {
		if it.Name == "" || it.ID == "" {
			continue
		}
		out = append(out, songItem{
			ID: it.ID, Mid: "", Name: it.Name,
			Artist: strings.Join(it.Artist, " / "), Album: it.Album, Source: "wy",
		})
	}
	return out
}

func searchCw(kw string, p, n int) []songItem {
	var r struct {
		Code int `json:"code"`
		Data []struct {
			ID     json.Number `json:"id"`
			Mid    string      `json:"mid"`
			Name   string      `json:"name"`
			Artist string      `json:"artist"`
			Album  string      `json:"album"`
		} `json:"data"`
	}
	out := make([]songItem, 0, n)
	if !cwPost("/music/search", map[string]interface{}{"keyword": kw, "page": p, "limit": n}, &r) || r.Code != 200 {
		return out
	}
	for _, d := range r.Data {
		if d.Name == "" || d.ID.String() == "" {
			continue
		}
		out = append(out, songItem{
			ID: d.ID.String(), Mid: d.Mid, Name: d.Name,
			Artist: d.Artist, Album: d.Album, Source: "wy",
		})
	}
	return out
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

	out := searchGd(kw, p, n)
	if len(out) == 0 {
		out = searchCw(kw, p, n)
	}
	if len(out) == 0 {
		return C.CString("[]")
	}
	js, err := json.Marshal(out)
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
	src := gdSource(C.GoString(csrc))
	br := strings.TrimSpace(C.GoString(cq))
	if br == "" || br == "master" || br == "hires" {
		br = "320"
	}

	cands := make([]string, 0, 2)
	if v := strings.TrimSpace(C.GoString(cid)); v != "" {
		cands = append(cands, v)
	}
	if v := strings.TrimSpace(C.GoString(cmid)); v != "" {
		cands = append(cands, v)
	}

	// ① gdstudio（实测唯一能给出真直链的通道）
	for _, id := range cands {
		params := url.Values{}
		params.Set("types", "url")
		params.Set("source", src)
		params.Set("id", id)
		params.Set("br", br)
		var r struct {
			URL string `json:"url"`
		}
		if gdGet(params, &r) && strings.HasPrefix(r.URL, "http") {
			if outLen != nil {
				*outLen = C.int(len(r.URL))
			}
			return C.CString(r.URL)
		}
	}
	// ② 老板的 key 通道（目前会返回 code=500，但留着：那是他们的服务端问题，恢复了就能用）
	for _, id := range cands {
		var r struct {
			Code int    `json:"code"`
			URL  string `json:"url"`
		}
		if cwPost("/music/url", map[string]interface{}{"source": "wy", "musicId": id, "quality": br}, &r) &&
			r.Code == 200 && strings.HasPrefix(r.URL, "http") {
			if outLen != nil {
				*outLen = C.int(len(r.URL))
			}
			return C.CString(r.URL)
		}
	}
	return C.CString("")
}

//export yieyu_music_version
func yieyu_music_version() C.int { return 3 }
