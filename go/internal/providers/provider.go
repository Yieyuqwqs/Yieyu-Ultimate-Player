// Package providers：搜歌源站的适配层（模块 5 的"手"）。
// 每个源站一个实现，只负责"给我一个关键词，还我一坨原始 JSON"。
// 洗数据不是这里的事（那是模块 6 的 Zod）。
package providers

import (
	"context"
	"net/http"
	"time"
)

// RawResult 是"脏数据"的容器：不解码、不校验，原样带回。
type RawResult struct {
	Source   string
	Keyword  string
	Body     []byte
	Err      error
	Elapsed  time.Duration
}

// Provider 只干一件事：发请求，拿字节。
type Provider interface {
	Name() string
	// Fetch 必须在 ctx 结束时立刻返回（并发扇出的超时由 fanout 统一控制）。
	Fetch(ctx context.Context, kw string) RawResult
}

var httpClient = &http.Client{
	Timeout: 8 * time.Second,
	Transport: &http.Transport{
		MaxIdleConns:        16,
		MaxIdleConnsPerHost: 4,
		IdleConnTimeout:     30 * time.Second,
		ForceAttemptHTTP2:   true,
	},
}

// registry 是全部源站。顺序=优先级，不代表并发顺序。
var registry = []Provider{
	&NetEase{}, &QQMusic{}, &Kugou{}, &Local{},
}

func All() []Provider { return registry }

// 三个占位实现：真接口签名在 P1 补，先把并发骨架跑通。
type NetEase struct{}
type QQMusic struct{}
type Kugou struct{}
type Local struct{}

func (p *NetEase) Name() string { return "netease" }
func (p *NetEase) Fetch(ctx context.Context, kw string) RawResult {
	return stubFetch(ctx, p.Name(), "https://music.163.com/api/search/get", kw)
}
func (p *QQMusic) Name() string { return "qq" }
func (p *QQMusic) Fetch(ctx context.Context, kw string) RawResult {
	return stubFetch(ctx, p.Name(), "https://c.y.qq.com/soso/fcgi-bin/client_search_cp", kw)
}
func (p *Kugou) Name() string { return "kugou" }
func (p *Kugou) Fetch(ctx context.Context, kw string) RawResult {
	return stubFetch(ctx, p.Name(), "https://mobilecdn.kugou.com/api/v3/search/song", kw)
}

// Local 只查本地库（模块 20/19 的 SQLite），不走网络。
func (p *Local) Name() string { return "local" }
func (p *Local) Fetch(ctx context.Context, kw string) RawResult {
	return RawResult{Source: p.Name(), Keyword: kw, Body: []byte(`{"songs":[]}`)}
}

func stubFetch(ctx context.Context, name, endpoint, kw string) RawResult {
	start := time.Now()
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, endpoint+"?keyword="+kw, nil)
	if err != nil {
		return RawResult{Source: name, Keyword: kw, Err: err, Elapsed: time.Since(start)}
	}
	req.Header.Set("User-Agent", "Mozilla/5.0 YieyuUltimate/0.1")
	resp, err := httpClient.Do(req)
	if err != nil {
		return RawResult{Source: name, Keyword: kw, Err: err, Elapsed: time.Since(start)}
	}
	defer resp.Body.Close()
	buf := make([]byte, 0, 32<<10)
	tmp := make([]byte, 8<<10)
	for {
		n, rerr := resp.Body.Read(tmp)
		if n > 0 {
			buf = append(buf, tmp[:n]...)
			if len(buf) > 4<<20 { // 单源上限 4MB，防被灌爆
				break
			}
		}
		if rerr != nil {
			break
		}
	}
	return RawResult{Source: name, Keyword: kw, Body: buf, Elapsed: time.Since(start)}
}
