# [7] Lua · 第三方音源脚本沙箱

落雪源（lx-music 生态）的脚本在这里跑。LuaJIT 是宿主，**但能力被砍到只剩一张白名单表**。

## 白名单（只有这些能用）

| 名称 | 用途 |
|---|---|
| `input` | 全局字符串，由宿主注入（搜索词 / 歌曲 id） |
| `http_get(url)` | 仅 http/https，8KB 返回上限（宿主内做） |
| `json.encode/decode` | 表 ↔ 字符串 |
| `base64.encode/decode` | — |
| `log(...)` | 进宿主日志，不进 stdout |

## 黑名单（注册时直接 `nil` 掉）

`io` · `os` · `package` · `dofile` · `loadfile` · `require` · `debug.sethook`

## 防死循环（两种模式）

| 模式 | 机制 | 代价 |
|---|---|---|
| 默认（进程内） | `lua_sethook` 指令计数，5000 万条/次调用 | 软掐，脚本里的 C 函数调用掐不住 |
| `--hard-kill` | 独立进程跑 VM，超时 `SIGKILL` | +12MB 内存，起进程 ~30ms |

## 静态检查

`tools/collect-lua.mjs` 在打包时扫源码，出现 `os.execute` / `io.open` / `package.loadlib` /
`debug.sethook` 直接构建失败。**这道闸在 CI 上也跑**，防止有人绕过注册表硬调。
