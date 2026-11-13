# HANDOFF · 接手必读（更新于 2026-11-13 深夜）

> 一句话：**24 模块骨架 + 全链路云端构建全部打通（run #47 绿，11m43s）**，
> 但**播放链路仍是空实现**，`libyieyu_core.so` 在真机上的加载状态**待确认**（见 §2）。

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

`run #47`（`19e6ecd`）success，11m43s。产物：

| 产物 | 大小 | 说明 |
|---|---|---|
| `libyieyu_core.so` | 714K | 含 LuaJIT（826K 静态链进来）+ 24 模块骨架 |
| `libyieyu_fetch.so` | ~5MB | Go 编的 C ABI HTTP（自带 TLS1.3/HTTP2） |
| `libyieyu_{gesture,license,store}.so` | 小 | Rust 三件套 |
| `libyieyu_mem.a` | 小 | Zig 分配器 |
| `YieyuUltimate-arm64.apk` | 16.5MB | 固定签名，可直接覆盖安装 |

### 2.2 文档构建的坑（**最后一次真机验证是失败状态**）

最后一次装机反馈：首页显示「出了点小状况，部分功能暂时不可用」→ 说明
`JsbClient.init()` 抛异常或返回非 0，**原生核心没加载起来**。

**已定位并修（run #45 起包含修复，run #47 复验构建仍绿；真机尚未复验）**：
`libyieyu_fetch.so`（Go 编）**没有 SONAME**，CMake 链接时把绝对路径写进
`libyieyu_core.so` 的 `NEEDED` 字段 → 装机后加载器按那个路径找库 → `dlopen` 失败 → 全盘降级。
修法：`go build -ldflags="-extldflags=-Wl,-soname,libyieyu_fetch.so"`，并在 CI 里加了
**15.5 动态依赖检查**（`NEEDED` 出现 `/` 就直接红）。

### 2.3 接手第一件事（按顺序）

```
1. 装 run #47 的 APK
2. 看首页顶部还有没有"出了点小状况"
   · 没有了 → 核心已加载，继续 §5 的 P2
   · 还在   → 设置页连续点标题 5 次 → 弹出启动日志原文（含 dlopen 的确切报错）
             把那段发给下一个人/模型，别猜
3. 设置 → 导入 Lua 脚本 → 选 .lua → 期望提示"脚本导入成功，已就绪"
```

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
| 1 | 播放链路：FFmpeg 解码 / SpeexDSP 重采样 / AAudio 出声，全是 stub | **能搜不能放**，这是最大的洞 | 3~5 天 |
| 2 | 搜歌结果没接回 UI（Go → TS 清洗 → JSB → 列表） | 搜索结果上不了屏 | 1~2 天 |
| 3 | `third_party/` 真实二进制（FFmpeg/SpeexDSP/Eigen/WasmEdge/SQLite/ORT） | 同上，纯 stub | 2~3 天 |
| 4 | Rive 资源 `.riv` 未做 | 动画走降级绘制（一条淡线） | 美术活 |
| 5 | `consumer-rules.pro` / gradle wrapper 未补 | 开混淆/runner 升级时有风险 | 0.5 天 |
| 6 | 18 ONNX 人声消除 | 未开始 | 3 天 |

---

## 6. 坑台账（27 条，全在 `docs/BLUEPRINT.md` §三）

最值钱的六条（都是"编译绿、装机黑"类型）：

1. `libc++_shared.so not found` → `ANDROID_STL=c++_shared` 时 gradle 不会自动打包，要手动从 NDK sysroot 拷进 `jniLibs`
2. `undefined symbol: Yieyu_Init` → `-fvisibility=hidden` 下 version script **提不起**符号，必须 `__attribute__((visibility("default")))`
3. `NEEDED` 里出现绝对路径 → Go 编的 .so 没 SONAME，装机必炸
4. `strstr` 扫无 `\0` 结尾的缓冲区 → 越界，导致"导入失败"
5. Gradle 8：`plugins {}` 之前不许有别的语句 → 签名块必须插在 `plugins{}` 之后
6. YAML `run:` 单行标量 + 缩进行 → 两行被折叠成一条命令（`chmod: cannot access 'chmod'`）

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

## 8. 常见故障对照

| 现象 | 先看哪里 |
|---|---|
| 首页「出了点小状况」 | 设置页连点标题 5 次 → 启动日志（dlopen 原因） |
| 导入失败 | 导出诊断日志 → 搜 `[7] 拒绝导入` / `[7] 导入失败` / `errno=` |
| 构建红在 native | 先看 CMake 报错行的 `#include` / Lua 5.1 API |
| 构建红在 flutter | 99% 是 `app/patch_gradle.mjs` 注入的 Groovy 语法问题 |
| 缓存 0 命中 | 看 step 20 的逐项命中；key 里千万别再放 `hashFiles` |
| 推送文档也触发构建 | 已修：`paths-ignore` 同时写 `*.md` 和 `**/*.md`（只写 `**.md` 匹配不到根目录） |
| job 根本不启动 | public/私有仓库计费问题（本仓库已 public，正常） |
