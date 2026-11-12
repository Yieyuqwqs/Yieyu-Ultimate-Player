# [5] Go · 协程并发搜歌

`goroutine` 是这里唯一的武器：每源站一个协程，整体预算 800ms，先到先出。

## 目录

| 路径 | 职责 |
|---|---|
| `internal/providers` | 源站适配（只发请求拿字节，不解析） |
| `internal/fanout` | 并发扇出 + 超时 + 排序合并 |
| `internal/bind` | **唯一的 AAR 出口**，只允许 gomobile 支持的类型 |
| `cmd/searchd` | 桌面调试入口（CI 里跑，不起 Android） |

## 三条边界

1. **不解析 JSON** —— 那是模块 6（TS + Zod）。
2. **不落盘、不碰 SQLite** —— 那是模块 19/20。`Local` 源只返回空壳，真查库由 core 侧拼。
3. **不长时间持有协程** —— 800ms 到点就走，慢源上报给 UI（`timeout` 字段）。

## 构建

```bash
go build ./...                        # 桌面验证
gomobile bind -target=android/arm64 -androidapi 26 -o ../out/go/search.aar ./internal/bind
```

> `gomobile bind` 会生成 Java/Kotlin 门面。**只有 `internal/bind` 这一层的 API 会露出去**，
> 这就是"极少量 Kotlin"能保住的原因。
