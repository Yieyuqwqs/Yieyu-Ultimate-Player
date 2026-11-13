# HANDOFF · 接手必读（更新于 2026-11-13 深夜）

> 一句话：**24 模块骨架 + 全链路云端构建全部打通（run #57 绿，15m）**；
> **播放链路 13→15 已打通**（平台解码 + AAudio 出声，见 §2.6），**真机听声待老板点一次**；
> `libyieyu_core.so` 的加载问题（Go 库没 SONAME）已修且产物实测无绝对路径依赖。

---

## 0. 30 秒上手

1. 读这份 `HANDOFF.md`
2. 读 `docs/BLUEPRINT.md` §0（日志与错误处理铁律）——**违反就算事故**
3. 读 `docs/ARCH_24.md`（24 模块表 = 架构真源）
4. 干活前先跑：`node ~/work/up/watch.js`（看最近一次构建）
5. 推代码只有一条路：`node ~/work/up/push.js`（本机没有 git，走 GitHub API 全量树提交）

---

## 1. 仓库与运行环境

| 项 | 值 |
|---|---|
| 仓库 | `Yieyuqwqs/Yieyu-Ultimate-Player`（**public**，为绕开私有仓库 Actions 计费） |
| 分支 | `main` |
| 本机工作副本 | `~/work/up/repo`（Android 手机上，**没有 git、没有 NDK/Rust/Go/Flutter**） |
| 唯一构建环境 | GitHub Actions（`.github/workflows/build.yml`） |
| 真机最低版本 | Android 26（AAudio 硬门槛） |
| 包名 | `com.yieyu.yieyu_ultimate` |

> **本机永远编不了**：NDK 不发布 aarch64-linux 宿主工具链，Flutter 不发 arm64 Linux SDK。
> 所有编译都在云端，本机只负责写文件 + 调 API。

---

## 2. 当前状态（最重要的一节）

### 2.1 构建：绿

`run #57`（`7858d1c`，2026-11-13 18:1x）success，15m（脚本→地址→出声整链 + Lua 网络语义 + 栈安全）。此前 #53/#52/#50/#48 均绿。
产物：

| 产物 | 大小 | 说明 |
|---|---|---|
| `libyieyu_core.so` | 729K | 含 LuaJIT（826K 静态链进来）+ 24 模块骨架 + 播放链路（13/15 已真实现） |
| `libyieyu_fetch.so` | ~5MB | Go 编的 C ABI HTTP（自带 TLS1.3/HTTP2） |
| `libyieyu_{gesture,license,store}.so` | 小 | Rust 三件套 |
| `libyieyu_mem.a` | 小 | Zig 分配器 |
| `YieyuUltimate-arm64.apk` | 16.5MB | 固定签名，可直接覆盖安装 |

### 2.2 文档构建的坑（**上一次真机验证是失败状态，修完还没复验**）

有一次装机反馈：首页显示「出了点小状况，部分功能暂时不可用」→ 说明
`JsbClient.init()` 抛异常或返回非 0，**原生核心没加载起来**。

**已定位并修（run #45 起包含修复，run #47 复验构建仍绿；真机尚未复验）**：
`libyieyu_fetch.so`（Go 编）**没有 SONAME**，CMake 链接时把绝对路径写进
`libyieyu_core.so` 的 `NEEDED` 字段 → 装机后加载器按那个路径找库 → `dlopen` 失败 → 全盘降级。
修法：`go build -ldflags="-extldflags=-Wl,-soname,libyieyu_fetch.so"`，并在 CI 里加了
**15.5 动态依赖检查**（`NEEDED` 出现 `/` 就直接红）。

> **2026-11-13 离线实测（run #50 产物，不靠真机）**：
> `DT_NEEDED` = libandroid/liblog/libdl/libaaudio/**libmediandk**/libm/libyieyu_fetch/libc++_shared/libc
> —— **全部是裸库名，没有绝对路径**；导出面 26 个符号（含新加的 6 个播放控制）齐全。
> 但"能 dlopen、能不能出声"只有真机能答，所以 §2.3 第 1 步仍是装机。

### 2.3 接手第一件事（按顺序）

```
1. 装 /sdcard/Download/YieyuUltimate-run57-arm64.apk（run #57 产物，固定签名，可直接覆盖旧包）
2. 看首页顶部还有没有"出了点小状况"
   · 没有了 → 核心已加载，继续 §5 的 P2
   · 还在   → 设置页连续点标题 5 次 → 弹出启动日志原文（含 dlopen 的确切报错）
             把那段发给下一个人/模型，别猜
3. 设置 → 导入 Lua 脚本 → 选 .lua → 期望提示"脚本导入成功，已就绪"
4. 设置页**连点标题 5 次** → 开发者面板 → 先点「本地测试音」听 3 秒 440Hz（不联网），再粘直链试真歌（§2.6）
   · 有声音 → 13→15 全链通；没声音 → 同一面板的「启动日志」里有原因
```

---

## 2.6 播放链路：平台解码后端 + AAudio 出声（2026-11-13 下午）

**目标**：先做到"能放歌"——**不引任何第三方二进制**就能出声。

| 层 | 文件 | 做什么 |
|---|---|---|
| [13] 解码 | `core/src/audio/demux_platform.c`（新） | AMediaExtractor 解封装 + AMediaCodec 解码 → float32 立体声交错；吃 http(s) 直链与本地路径 |
| [13] 兜底 | `core/src/audio/demux_ffmpeg.c`（没动） | 有 FFmpeg 预编译库时仍走它；`YIEYU_OFFLINE=ON` 时是空实现 |
| [15] 出声 | `core/src/audio/aaudio_sink.c`（重写） | 改成"拉数据回调"模型；独占→共享四级降级；PCM float 立体声 |
| 调度 | `core/src/pipeline/player.{h,cpp}`（新） | 唯一解码线程 + PCM 环（131072 float ≈ 1.4s）+ 暂停/继续/停止/seek/音量；音频回调零锁零分配零日志 |
| 导出 | `core/src/jsb/jsb.{h,cpp}` + `core/cmake/exports.map` | `Yieyu_PlayUrl / Pause / Resume / Stop / SeekMs / SetVolume`（**异步受理**：返回 0 只是"请求收到了"） |
| 试听入口 | `app/lib/diag/dev_panel.dart`（新） | 设置页标题连点 5 次 → 开发者面板 → 填直链 → 播放 / 暂停 / ±10s / 音量 |

**线程模型（守铁律 2，别改回去）**：
Dart/JSB 线程只写原子请求 → 唯一解码线程做阻塞打开与解码 → 音频回调只 `Pull()`（读环 + 算增益/峰值）。

**真机定位顺序（有毛病时按这个走，别猜）**：
1. 面板点「本地测试音」→ 听到 3 秒 440Hz = **[13] 解码 + [15] AAudio + 解码线程全通**；
   没声音 → 看状态行的 `st=`（0=空闲 / 1=打开中 / 2=播放 / 3=暂停 / 4=出错）与 `underrun=`；
2. 测试音正常、直链不正常 → 锅在 **URL/网络/格式**（先试 `/sdcard/Music` 下的本地 FLAC 路径）；
3. 两者都没声 → 锅在 [15]：去 设置 → 导出诊断日志 → 搜 `[15]`（里面会写是独占失败还是全失败）。

**已知边界（不是 bug，别当 bug 报）**：
- **不重采样**：AAudio 直接按源采样率开流（44.1k 就是 44.1k），[14] SpeexDSP 还没接；
- **不接 DSP**：[17] 的 EQ / 胆机 / 黑胶还没进链路，先听到声音再叠；
- **seek** 走 `AMEDIAEXTRACTOR_SEEK_CLOSEST_SYNC`（按关键帧对齐），界面位置按"已播帧数"折算；
- **播完不自动下一首**（"播什么"是模块 1 的决策，不归播放器）；
- **FLAC/APE 等高码率**：平台解码器支持面取决于系统（Android 自带 FLAC/ALAC 软解，APE 一般不支持）→ 那种文件才需要 FFmpeg。

---

### 2.7 「脚本导入失败」的真因（2026-11-13 下午，老板报障）

真机现象：设置 → 导入 Lua 脚本 → 提示失败。

**现场取证（这一步以后照着做，别猜）**：
1. `/storage/emulated/0/yieyu/scripts/` 里只有 09:53 的 `yieyu.lua`，**没有新文件**；
2. `/storage/emulated/0/yieyu/` 里**没有任何 .log**（= 原生侧没记过 ERROR）——
   说明失败**还没进到原生**，或者原生写了但只写进了私有目录。

**结论（最可能）**：App 在 14:08 被重装过一次。**重装会撤销「所有文件访问」（MANAGE_EXTERNAL_STORAGE）**，
于是 `/storage/emulated/0/yieyu/scripts` 写不进去 → `Yieyu_ImportScript` 返回 -3
→ 界面只显示「脚本导入失败，请重试」，而真正的 errno 被埋进（现在在私有目录的）黑匣子日志。

**已修（结构性问题，不是补权限就完事）**：
- 新增 `scripts_dir()`：**公共目录可写就用公共，不可写就落到 `<filesDir>/yieyu/scripts`**；
- `lua_sandbox` 新增 `lua_sandbox_add_dir()`：重载时**两个目录都扫**（同名以先扫到的为准）；
- 导入链路每一步都追加一行到 `/storage/emulated/0/yieyu/import_diag.txt`
  （选择器返回空 / 名字不是 .lua / 读到空内容 / rc=… / 异常）——**能写进去就是权限还在，写不进去本身就是诊断**；
- 开发者面板新增「脚本导入（诊断）」卡：按路径导入 / 重新加载脚本 / 公共目录可写？ 三个按钮，绕开 SAF 选择器。

> 顺手确认：老板放在 `/sdcard/` 的那个「一键规范化+渲染音源.lua」是 **REAPER 的脚本**（全是 `reaper.*` API），
> 不是本 App 的音源脚本；本 App 要的是落雪生态的 `get_music_url` / 搜索类脚本
> （现成的在 `/storage/emulated/0/yieyu/scripts/yieyu.lua`）。

### 2.8 [17] DSP 音效链接入（2026-11-13 傍晚，run #54 起）

**做了什么**：新增 `core/src/dsp/dsp.h`，把原本「写好了但没人调」的三件套接进**解码线程**
（解码 → EQ31 → 胆机/黑胶 → 8D → PCM 环 → AAudio）：

| 环节 | 函数 | 默认 |
|---|---|---|
| 31 段 EQ（ISO 1/3 倍频程 biquad） | `eq31_init/eq31_process/eq31_set_gain` | 旁路（全 0 dB） |
| 胆机软削波 + 黑胶 + 16k 陡降 LPF | `tube_vinyl_process/tube_vinyl_set` | 旁路 |
| 8D 空间化（吃 [23] 姿态） | `hrtf8d_process/hrtf8d_set_pose` | 旁路 |

**为什么默认旁路**：音效会改变音色，在真机确认「能出声」之前一律不动声音；
JSB 新增 `Yieyu_SetDsp(on)` / `Yieyu_SetEqGain(band, db)` / `Yieyu_SetTone(tube,vinyl,lpf,drive)`，
开发者面板加了「[17] 音效链」开关 + 「低音+6dB」按钮（打开开关后按下去，低音明显变重 = [17] 真的在跑）。

**红线**：DSP 只在**解码线程**跑。AAudio 回调里仍然只有「读环 + 音量 + 峰值」（铁律 2 不变）。

---

### 2.9 Lua 脚本规约（2026-11-13 傍晚，run #55 起）

#### 网络函数（白名单里就这些）
| 名字 | 语义 | 返回 |
|---|---|---|
| `http_get(url[, headers])` | GET | `body, status` |
| `http_post(url, body \| headers, …)` | POST，**两种参数顺序都吃** | `body, status` |
| `request(arg)` / `fetch(url[, …])` | 同 GET；`request{url=,method=,body=,headers=}` 表形式也支持 | `body, status` |
| `http.get/post/request` | 同上（表形式别名） | `body, status` |
| `json.decode/encode` | 自研实现（Lua 5.1 兼容） | — |

**三个必须记住的点（都是踩过的）**：
1. `http_post` 的 headers 可以是 **Lua 表**（`{["X-Api-Key"]="…"}`），也可以是一行一行的 `"K: V"` 字符串；
2. **第二个返回值是 HTTP 状态码**（网络层失败 = 0）。脚本写 `local body, code = http_post(...)`、
   `if code ~= 200 then return nil end` 才能正常工作 —— 老实现只回 body，code 是 nil，判断永远成立；
3. Go 侧出口是 `yieyu_fetch_ex(url, method, body, headers, outLen, outStatus)`（`yieyu_fetch` 旧签名保留）。

#### 沙箱边界
- 删掉：`io` / `package` / `loadfile` / `dofile`（`require` 随之失效）；
- `os` **保留**，只砍 `execute/exit/remove/rename/tmpname/getenv/setlocale` → `os.time()`、`os.date()` 可用；
- 静态黑名单（导入时就拦）：`os.execute/os.exit/io.open/io.write/package.loadlib/debug.sethook/require(/dofile(/loadfile(`；
- 指令计数上限 5000 万条/次，超了软掐，不会把 App 拖死。

#### 文件路径规约
- **导入**：Flutter 只把**字节**交给 `Yieyu_ImportScript(name, bytes, len)` —— 系统选择器给的
  `content://` 只是个句柄，C++ 读不到，所以永远不要传 URI 下去；
- **播放本地文件**：开发者面板「选本地音频」→ 选择器读字节 → **写进 App 私有缓存** → 把**绝对路径**交给
  `Yieyu_PlayUrl`；
- 脚本目录：公共目录可写就写公共（用户能直接放），否则落 `<filesDir>/yieyu/scripts`；重载时两处都扫。

---

### 2.10 脚本 → 播放地址 → 出声（2026-11-13 傍晚，run #56 起）

**之前的状态（这就是"导入了也没用"的原因）**：`lua_sandbox_run` / `lua_sandbox_call`
**全仓没有任何调用点** —— 沙箱能导入、能校验、能从 Lua 发 HTTP，但**没人让它执行**，
所以脚本里写的 `get_music_url()` 永远不会被调到。

**这次补上的链路**：
```
Dart 面板「调用 get_music_url」
  → Yieyu_ScriptCall(script, fn, a1, a2, a3, out, cap)     [24] JSB
  → lua_sandbox_call()：按名字载入脚本 → 取全局函数 → 压参数 → pcall
  → 脚本内 http_post(url, headers表, body)  ← 两种参数顺序都吃，返回 (body, status)
  → Go yieyu_fetch_ex(...) 带状态码
  → 返回 URL → 面板「播放该地址」→ Yieyu_PlayUrl → [13] 解码 → [15] AAudio
```

**返回值约定**（`lua_sandbox_call` / `Yieyu_ScriptCall`）：
`0` 成功 / `-1` 参数错 / `-2` 脚本载入失败 / `-3` 没有这个函数 / `-4` 运行时报错；
失败原因进 `Yieyu_LastError()`，开发者面板和 `import_diag.txt` 都能看到（正常界面依旧静默）。

**老板那份脚本（`yieyu.lua`）逐条核对结果**：
- 黑名单 9 条**一条都没命中** → 导入失败与脚本内容无关；
- `http_post(target_url, headers, body)` 的表头 + 参数顺序 → 老实现会 `luaL_checkstring(表)` 直接报错；
- `local body, code = http_post(...)` 的第二个返回值 → 老实现只回 body，`code` 是 nil，
  `if code ~= 200 then return nil end` 永远成立 → 永远拿不到 URL。两处都在 #55 修掉；
- `on_init()` 是自定义函数，宿主不会自己调（要调就走 `Yieyu_ScriptCall(script, "on_init", NULL, NULL, NULL, out, cap)`）。

---

### 2.11 路径唯一真相（2026-11-13 晚，根因修复；**取代 §2.7 的分段补丁**）

#### 根因（一句话）
「往哪写」这件事原来在三处各做一遍：
`blackbox` 自己探测公共目录 → `jsb` 自己拼 scripts 路径（还分公共/私有两条）→ 裸 `::mkdir` 只建一层，
于是「重装 App 丢了『所有文件访问』」这一个外部变化，被放大成 3 处失败叠加，
先表现为「导入失败但没有日志」，修完权限兜底又变成「私有目录没建出来 → rc=-3」。

#### 现在只有一个决定点：`core/src/common/app_paths.{h,cpp}`
| 函数 | 语义 |
|---|---|
| `yp_init(dataDir)` | 在 `Engine::Boot` 第一句调用，**全进程只调一次**；dataDir = `getApplicationSupportDirectory()` |
| `yp_home()` | 运行根：公共 `/storage/emulated/0/yieyu` **真的可写**就用它，否则 `<data>/yieyu`；init 时递归建好 |
| `yp_scripts()` | `<home>/scripts`；脚本只往这里写，只从这里扫 |
| `yp_data()` | 私有目录（SQLite 只能放这里） |
| `yp_ensure()` | **全工程唯一的 mkdir 实现**（递归、幂等） |
| `yp_writable()` | 真写探针文件判断可写（`access(W_OK)` 在 Android 上会骗人） |

配套清理（都是同一根因的产物）：
- `blackbox`：`bb_init(dir)` 只收一个目录，不再探测、不再有 fallback 参数；
- `jsb`：删掉 `scripts_dir()` / `legacy_scripts_dir()` / 自建 `ensure_dir()` / 双目录扫描，
  导入只写 `yp_scripts()`，写完**立刻回读**验证；
- 黑名单：`lua_sandbox_banned_text()` 成为**唯一实现**（jsb 不再维护第二份数组）；
- Dart：不再出现任何 `/storage/…` 硬编码，路径一律问 `Yieyu_LogDir`（= `yp_home()`）。

#### 诊断接口（真机自查顺序）
1. 设置标题连点 5 次 → 开发者面板 →「运行目录」按钮：会显示目录 + 在公共还是私有；
2. 导入脚本 → 失败时面板直接显示 `Yieyu_LastError()` 的原因（errno / 黑名单项 / loaded=0）；
3. 面板「调用 get_music_url」验证脚本执行，或「本地测试音」验证 [13]+[15]。

---

---

## 3. 本机工具脚本（都在 `~/work/up/`）

| 脚本 | 作用 | 用法 |
|---|---|---|
| `push.js` | **推送**：全量树提交（支持删除），`.sh/.mjs` 自动给 100755 | `node push.js` |
| `watch.js` | 盯最近一次构建到结束，打印每步 ✓/✗ | `node watch.js` |
| `logs.js` | 拉失败日志（自动跟随重定向） | `node logs.js <run_id>` |
| `fetch-apk.js` | 断点续传下载 artifact（移动网络会断） | `node fetch-apk.js <artifact_id> <输出路径>` |
| `api.js` | GitHub API（IP 直连 + 重试 + token 从 `~/tools/.gh_token` 读） | `require('./api')` |
| `get-needed.mjs` / `fetchlog.mjs` | 抓构建日志里特定段落 | 直接用 |

触发构建（**别和 push 同时做，会互相 cancel**）：
```bash
node -e 'const{request}=require("./api");request("POST","/repos/Yieyuqwqs/Yieyu-Ultimate-Player/actions/workflows/build.yml/dispatches",{ref:"main"}).then(()=>console.log("ok"))'
```

---

## 4. 架构速览（细节见 docs/）

```
app/       Flutter 顶层（模块 1/2）+ ffi 绑定（3/24）+ 设置页（导入脚本/导出日志）
core/      C++ 汇聚层 → libyieyu_core.so（模块 7/8/9/12~18/20/21/23/24）
rust/      Cargo workspace（3 手势 / 10 验签 / 19 只读库）
go/        5 搜歌（AAR）+ fetch/（C ABI HTTP，给 Lua 用）
ts/        6 Zod 洗数据 → out/ts/cleaner.mjs
zig/       4 分配器 → libyieyu_mem.a
asm/       12 NEON 常数时间比对
lua/       7 脚本沙箱的 API 文档 + 示例
wasm/      8 插件示例（guest）
python/    18 模型训练/导出（不进手机）
android/   22 唯一 Kotlin（前台服务，预算 400 行）
idl/       21 FlatBuffers 契约（jsb.fbs / frames.fbs）
docs/      ARCH_24 / DATAFLOW / BUILD / CONTRACTS / ROADMAP / BLUEPRINT
```

**谁汇聚谁**：所有语言产物最终链进 `libyieyu_core.so`；Flutter 只直接 FFI `core`/`gesture`/`store`。

---

## 5. 已完成 vs 未完成

### ✅ 已完成（真跑通，不是纸面）

- 24 模块目录 + 契约 + 构建总控（`build.sh` → `tools/orchestrate.mjs`，DAG + 指纹增量）
- 云端工具链：NDK / Rust / Zig / Flutter / Go / flatc / gomobile / ccache / LuaJIT，13 缺 → 0 缺
- 五层缓存（out / ccache / sccache / go / gradle+pub），无改动重跑从 15 分钟降到分钟级
- **固定签名**（`app/signing/yieyu.jks` 入库）→ APK 可直接覆盖安装
- **黑匣子日志**：5MB 轮转 ×3、异步线程、trace_id、`_Unwind_Backtrace` 堆栈、UI 全静默
- **Lua 脚本热更**：SAF 导入 → 黑名单校验 → 落盘 → 注册 → LuaJIT 真执行
- **脚本 HTTP**：`http_get/http_post/request/fetch/http.*` 走 `libyieyu_fetch.so`（Go+TLS）
- **JSON**：自研 `json.decode/encode`（Lua 5.1 兼容）
- 符号治理：`exports.map` 白名单 + `visibility("default")` + CI 硬校验

### ❌ 未完成（按优先级）

| # | 缺口 | 影响 | 工作量估计 |
|---|---|---|---|
| 1 | ~~播放链路~~ → **已通最小闭环**（平台解码 + AAudio 出声，见 §2.6）；缺 [14] 重采样 / [17] DSP / FFmpeg 后端 | 已经能放歌，音效链还没接 | 2~3 天 |
| 2 | 搜歌结果没接回 UI（Go → TS 清洗 → JSB → 列表） | 搜索结果上不了屏 | 1~2 天 |
| 3 | `third_party/` 真实二进制（FFmpeg/SpeexDSP/Eigen/WasmEdge/SQLite/ORT） | 同上，纯 stub | 2~3 天 |
| 4 | Rive 资源 `.riv` 未做 | 动画走降级绘制（一条淡线） | 美术活 |
| 5 | `consumer-rules.pro` / gradle wrapper 未补 | 开混淆/runner 升级时有风险 | 0.5 天 |
| 6 | 18 ONNX 人声消除 | 未开始 | 3 天 |

---

## 6. 坑台账（29 条，全在 `docs/BLUEPRINT.md` §三）

最值钱的六条（都是"编译绿、装机黑"类型）：

1. `libc++_shared.so not found` → `ANDROID_STL=c++_shared` 时 gradle 不会自动打包，要手动从 NDK sysroot 拷进 `jniLibs`
2. `undefined symbol: Yieyu_Init` → `-fvisibility=hidden` 下 version script **提不起**符号，必须 `__attribute__((visibility("default")))`
3. `NEEDED` 里出现绝对路径 → Go 编的 .so 没 SONAME，装机必炸
4. `strstr` 扫无 `\0` 结尾的缓冲区 → 越界，导致"导入失败"
5. Gradle 8：`plugins {}` 之前不许有别的语句 → 签名块必须插在 `plugins{}` 之后
6. YAML `run:` 单行标量 + 缩进行 → 两行被折叠成一条命令（`chmod: cannot access 'chmod'`）
7. **release 包没有 INTERNET 权限** → 装机后所有网络静默失败（直链放不出来、Lua `http_get` 返回空）。
   `flutter create` 只给 debug/profile 壳子加，已由 `app/patch_manifest.mjs` 注入（踩过一次）
8. **AAudio 独占模式真机常被拒** → 必须按 独占+burst → 独占 → 共享+burst → 共享 四级降级，
   一上来就死等于装机无声；另外 `AMediaCodec` 的 `pcm-encoding` 只有在 `OUTPUT_FORMAT_CHANGED` 后才可信

---

## 7. 红线（谁改谁负责）

详见 `docs/ARCH_24.md` §0 与 `docs/BLUEPRINT.md` §0：

1. 一个模块只干表里那件事，不许跨界
2. 音频线程零 JNI / 零 Dart / 零分配 / 零日志
3. 同一个 SQLite 文件只有一个写者（M20 写，M19 只读）
4. 卡密密钥只在 M10，其它任何地方出现副本 = 安全事故
5. Kotlin 总量 ≤ 400 行（CI 会查）
6. **UI 绝对静默**：任何技术报错文本出现在界面 = 安全事故
7. **成功流程不记日志**；`LOGI/LOGW` 是编译期 no-op
8. 只有 ERROR/FATAL 落盘，5MB 轮转 ×3，异步线程

---

## 7.5 签名密钥的血泪史（务必别重演）

`app/signing/yieyu.jks` 是 **CI 用 keytool 生成后 commit 回仓库**的，本机原本没有副本。
而我们推送用的是"全量树"提交（支持删除文件）→ **每次 push 都会把它删掉** →
下一次 CI 的 `0.3` 步骤发现文件不存在，又生成**新的一把** → 签名变了 → 覆盖安装失败。

现在的防护：
1. 本地已把这个文件拉回来（`node pull-key.mjs`），推送时会原样带上；
2. `push.js` 里加了 `PROTECTED` 检查：远端有、本地缺 → **直接中止提交**。

> 历史上已经因此换过三把密钥。真要换密钥，必须同时：
> ①改 `app/signing/README.md` ②告知用户卸载重装 ③把旧 key 归档。

## 8. 常见故障对照

| 现象 | 先看哪里 |
|---|---|
| 首页「出了点小状况」 | 设置页连点标题 5 次 → 启动日志（dlopen 原因） |
| 导入失败 | 导出诊断日志 → 搜 `[7] 拒绝导入` / `[7] 导入失败` / `errno=` |
| 构建红在 native | 先看 CMake 报错行的 `#include` / Lua 5.1 API |
| 构建红在 flutter | 99% 是 `app/patch_gradle.mjs` 注入的 Groovy 语法问题 |
| 缓存 0 命中 | 看 step 20 的逐项命中；key 里千万别再放 `hashFiles` |
| 推送文档也触发构建 | `paths-ignore` 要同时写 `*.md` 和 `**/*.md`（只写 `**.md` 匹配不到根目录） |
| **每次构建签名都变、装不上** | ⚠️ 曾因"全量树推送"把 CI 生成的 `app/signing/yieyu.jks` 删掉，CI 又用 keytool 生成新的一把。**现已修**：本地保留该文件 + `push.js` 加保护检查（远端有、本地缺 → 直接中止） |
| job 根本不启动 | public/私有仓库计费问题（本仓库已 public，正常） |
