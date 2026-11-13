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
| `json.decode(str)` | 字符串 → 表（嵌套上限 32，非 ASCII 的 \u 转义简化为 ?） |
| `json.encode(value)` | 表 → 字符串 |

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
| Lua 执行引擎 | ✅ **已接入**（CI 自动编 LuaJIT 826K，编不出自动降级为 stub） |
| `http_get/http_post` 真发请求 | ✅ **已接通**：走 `libyieyu_fetch.so`（Go + TLS1.3/HTTP2，不依赖 OpenSSL） |

**所以现在导入会成功、脚本会被校验和登记，但"导入完就能搜歌"要等 curl 接上。**


## HTTP 是怎么实现的（模块 9 的替代方案）

`go/fetch/main.go` → `libyieyu_fetch.so`（`-buildmode=c-shared`），C ABI：

```c
char* yieyu_fetch(url, method, body, headers, int* outLen);
void  yieyu_fetch_free(char* p);
```

- 自带 TLS 1.3 / HTTP2 / 连接池，**不需要 OpenSSL 或 mbedTLS**
- 单次响应上限 256KB，超时 12s
- 只允许 http(s)，与沙箱白名单一致
- 沙箱没链接到它时自动退化为返回空串（不崩）


## 最小可用脚本模板（直接可用）

```lua
-- 把源站原文交给宿主清洗，自己不要解析（源站改字段时崩的是清洗层，不是你的脚本）
function search(kw)
  local url = "https://example.com/api/search?kw=" .. kw
  local raw = http_get(url)
  if raw == "" then return nil end        -- 网络失败：返回 nil，宿主会提示"网络开小差了"
  return json.decode(raw)                 -- 需要自己解字段时用 json.decode
end

function get_url(song)
  local raw = http_post("https://example.com/api/url", json.encode({ id = song.id }))
  if raw == "" then return nil end
  return json.decode(raw).url
end

return { search = search, get_url = get_url }
```

> 判断约定：`http_get` 失败返回 **空字符串**（不是 nil），所以用 `== ""` 判断。
