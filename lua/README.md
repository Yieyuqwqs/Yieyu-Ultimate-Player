# [7] Lua 音源脚本沙箱

## 你能用的 API（就这些，没有别的）

| 写法 | 说明 |
|---|---|
| `http_get(url)` | GET，返回 body 字符串 |
| `http_post(url, body [, headers])` | POST，返回 body 字符串 |
| `request(url)` / `fetch(url)` | `http_get` 的别名（兼容不同脚本习惯） |
| `http.get` / `http.post` / `http.request` | 表形式的别名 |
| `input` | 全局字符串，宿主注入（搜索词 / 歌曲 id） |
| `log(...)` | 写黑匣子（**只有出错才落盘**） |

> 之前只注册了 `http_get`，用 `http_post` 的脚本会直接报 nil —— 已修复，四种写法都指向同一个出口。

## 脚本返回"脏数据"，不返回"干净数据"

沙箱**不解析 JSON**。你的脚本把源站原文（或直链）返回给宿主，清洗交给模块 6（TS + Zod）。

这样设计的好处：源站改字段时，崩的是清洗层（有兜底、有测试），不是你的脚本，也不会把主程序带崩。

## 黑名单（静态检查，命中即拒绝导入）

`os.execute` · `os.exit` · `io.open` · `io.write` · `package.loadlib` ·
`debug.sethook` · `require(` · `dofile(` · `loadfile(`

注册表层面这些库已被置 nil，静态检查是第二道闸。

## 防死循环

指令计数 hook：每次执行上限 **5000 万条**，超限直接 `luaL_error` 掐断。
（C 函数内部死循环掐不住，那种情况要靠 `--hard-kill` 独立进程模式，P3 做。）

## 导入方式（App 内）

设置 → **导入 Lua 脚本** → 系统文件选择器选中 `.lua` → 立刻落盘到
`<日志目录>/scripts/<name>.lua` 并注册进沙箱。

沙箱目录跟黑匣子走：有全盘权限就是 `/storage/emulated/0/yieyu/scripts/`，
没有就自动落到 App 私有目录。

## 现状（别自欺）

| 环节 | 状态 |
|---|---|
| 导入 / 黑名单 / 登记 | ✅ 已可用 |
| Lua 执行引擎 | ⚠️ 需要 `third_party/lua`（CI 里已加 LuaJIT 自动编译，编不出就降级） |
| `http_get/http_post` 真发请求 | ❌ 还要接模块 9 的 libcurl（P2） |

**所以现在导入会成功、脚本会被校验和登记，但"导入完就能搜歌"要等 curl 接上。**
