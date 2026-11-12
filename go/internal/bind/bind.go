// Package bind：模块 5 唯一暴露给外部的面（gomobile bind 的边界）。
//
// 铁律：这一层只允许 gomobile 支持的类型（string/int/bool/[]byte/error）。
// 业务包（fanout/providers）不许直接出现在签名里 —— 否则 AAR 的 API 面会失控。
package bind

import (
	"context"
	"time"

	"github.com/Yieyuqwqs/Yieyu-Ultimate-Player/go/internal/fanout"
)

// Version 让 Kotlin/Dart 侧能自检 AAR 是否对得上。
func Version() string { return "0.1.0" }

// SearchRaw 返回"脏 JSON"（源站原文 + 用时），交给模块 6 的 Zod 洗。
// budgetMs <= 0 时用默认 800ms。
func SearchRaw(keyword string, budgetMs int) (string, error) {
	if budgetMs <= 0 {
		budgetMs = 800
	}
	// 注意：nil context 会让 fanout 里的 context.WithTimeout panic → 必须传 Background
	res, err := fanout.Search(context.Background(), keyword, time.Duration(budgetMs)*time.Millisecond)
	if err != nil {
		return "", err
	}
	b, err := fanout.MarshalTraces(res.Raw)
	if err != nil {
		return "", err
	}
	return string(b), nil
}
