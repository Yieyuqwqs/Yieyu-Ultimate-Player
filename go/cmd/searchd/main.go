// searchd：桌面/CI 上直接跑搜歌链的调试入口（不依赖 Android）。
// 用法：go run ./cmd/searchd "关键词"
package main

import (
	"context"
	"encoding/json"
	"fmt"
	"os"
	"time"

	"github.com/Yieyuqwqs/Yieyu-Ultimate-Player/go/internal/fanout"
)

func main() {
	if len(os.Args) < 2 {
		fmt.Fprintln(os.Stderr, "用法: searchd <关键词> [budgetMs]")
		os.Exit(2)
	}
	kw := os.Args[1]
	budget := 800 * time.Millisecond
	start := time.Now()
	res, err := fanout.Search(context.Background(), kw, budget)
	if err != nil {
		fmt.Fprintln(os.Stderr, "扇出失败:", err)
		os.Exit(1)
	}
	out := map[string]any{
		"sources":   len(res.Raw),
		"timeout":   res.Timeout,
		"elapsedMs": time.Since(start).Milliseconds(),
	}
	b, _ := json.MarshalIndent(out, "", "  ")
	fmt.Println(string(b))
}
