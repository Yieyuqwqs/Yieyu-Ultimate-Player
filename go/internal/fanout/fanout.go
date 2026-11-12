// Package fanout：并发扇出 + 先到先出 + 合并去重。模块 5 的心脏。
package fanout

import (
	"context"
	"encoding/json"
	"sort"
	"sync"
	"time"

	"github.com/Yieyuqwqs/Yieyu-Ultimate-Player/go/internal/providers"
)

// Result 是洗数据之前的中间态。
type Result struct {
	Raw     []providers.RawResult
	Timeout int // 超时的源站数（要上报给 UI："3 个源中 1 个慢"）
}

// Search 向所有源站同时发请求，整体预算 budget（默认 800ms）。
// 慢的源站不等 —— 用户要的是"秒出"，不是"全出"。
func Search(ctx context.Context, kw string, budget time.Duration) (Result, error) {
	ctx, cancel := context.WithTimeout(ctx, budget)
	defer cancel()

	ps := providers.All()
	var (
		mu  sync.Mutex
		out = make([]providers.RawResult, 0, len(ps))
		wg  sync.WaitGroup
		to  int
	)
	for _, p := range ps {
		wg.Add(1)
		go func(p providers.Provider) {
			defer wg.Done()
			r := p.Fetch(ctx, kw)
			mu.Lock()
			defer mu.Unlock()
			if r.Err != nil {
				to++
				return
			}
			out = append(out, r)
		}(p)
	}
	wg.Wait()

	// 快源在前（用户先看到有内容的），稳的源补后
	sort.SliceStable(out, func(i, j int) bool { return out[i].Elapsed < out[j].Elapsed })
	return Result{Raw: out, Timeout: to}, nil
}

// Trace 是给 [6] TS 的交接格式：源站名 + 原始字节的 base64 无关形式。
// 这里刻意保持"脏"——清洗是模块 6 的唯一职责。
type Trace struct {
	Source  string `json:"source"`
	Elapsed int64  `json:"elapsedMs"`
	Body    string `json:"body"`
}

func MarshalTraces(rs []providers.RawResult) ([]byte, error) {
	tr := make([]Trace, 0, len(rs))
	for _, r := range rs {
		tr = append(tr, Trace{Source: r.Source, Elapsed: r.Elapsed.Milliseconds(), Body: string(r.Body)})
	}
	return json.Marshal(map[string]any{"traces": tr})
}
