# 历史台账（本文只作为"踩过什么坑"的记录）

> ⚠️ 本文里的"待办/P2 要做/third_party 空实现"等段落是**当时的状态**，不是当前状态。
> 当前状态以 [`../README.md`](../README.md) 与 [`../HANDOFF.md`](../HANDOFF.md) 为准。

# 总纲 · 今日战报与下一步

> **状态：云端全链路绿了，APK 装机跑起来了。**
> `run #20`（`744b8b6`）success，12/12 步全绿；APK 57.4MB，装上后红色降级条消失 =
> `libyieyu_core.so` 真正加载 + `Yieyu_Init` 调用成功。

---

## 〇、日志与错误处理铁律（凌驾于一切之上）

> 这一节是**硬约束**：review 只看这一条，违反就回退。

### 1. UI 绝对静默

底层发生任何事（C++ 崩溃、Go 超时、Rust 验签失败、dlopen 失败、SQLite 锁死），
**界面上不许出现任何红条、异常原文、堆栈、模块号、文件路径**。

| 场景 | UI 只许说 |
|---|---|
| 启动失败 / 原生库没加载 | 「出了点小状况，部分功能暂时不可用」 |
| 搜歌失败 | 「网络开小差了，请重试」 |
| 播放失败 | 「这首暂时放不了」 |
| 授权失败 | 「请先完成授权」 |

实现：`app/lib/main.dart` 的 `_degraded` 只喂一个布尔给 UI，**异常对象在 catch 里就地丢弃**。

> 已经删过一次违反此条的代码：早期版本会把
> `dlopen failed: library "libc++_shared.so" not found ...` 直接怼在屏幕顶上。

### 2. 静默成功，仅记 ERROR

`LOGI` / `LOGW` 在 `core/src/common/log.h` 里是 **`((void)0)`**，编译期就消失。
成功的流程一行日志都不产生 —— 音频线程上的 I/O 与内存增长是不可接受的成本。

只有 `LOGE`（ERROR）和 `LOGF`（FATAL）进黑匣子。

### 3. 物理黑匣子（异步落盘）

| 项 | 规定 |
|---|---|
| 路径 | 首选 `/storage/emulated/0/yieyu/yieyu_log.txt` |
| 降级 | 没拿到存储权限 → `filesDir/yieyu/`（App 私有），并记一条 ERROR 说明降级 |
| 写入 | 独立后台线程；调用方只做一次 memcpy 入队（单条 ≤3KB，队列 128，**满了丢**） |
| 阻塞 | 绝不阻塞 UI / 音频线程；`bb_flush()` 最多等 2s，只在导出/退出时调 |
| 每条 | `时间戳 \| 级别 \| trace_id \| 模块 \| 消息(含入参)` + 下一行 `stack: frames=N 0x..` |
| trace_id | 每条 JSB 命令生成一个，贯穿整条链路 |
| 堆栈 | `_Unwind_Backtrace` 取 16 帧，只存地址（不 symbolicate，避免额外依赖） |

### 4. 防爆表轮转

- 单文件上限 **5MB**
- 写满 → `yieyu_log.txt` → `yieyu_log.txt.1`
- 最多保留 **3** 个历史（`.1 .2 .3`），最旧的在轮转时删除
- 轮转在写线程里做，UI 无感

### 5. 导出接口

设置页（右上角 ⋮）→ **「导出诊断日志」** 一个按钮，没有别的技术选项。

- 点击 → `Yieyu_ExportLogs()` → `bb_flush()` → 当前 + 3 个历史文件拷到 `<日志目录>/exported/`
- UI 只回「已导出，可在文件管理器里查看」或「导出没成功，请重试」
- **不显示路径**

> 权限现实：Android 11+ 分区存储下普通 App 无权直接写 `/storage/emulated/0/yieyu/`。
> `app/patch_manifest.mjs` 构建时向壳子注入 `MANAGE_EXTERNAL_STORAGE`；
> 用户给「所有文件访问」就写公共目录，不给就自动降级，功能不受影响。

## 一、今天到底做成了什么

| 项 | 结果 |
|---|---|
| 仓库 | `Yieyuqwqs/Yieyu-Ultimate-Player`（private，main） |
| 24 模块骨架 | 99 → 101 个文件全部落位，序号 1~24 一一对应 |
| 云工具链 | 13 个缺失工具 → **0 缺失**（NDK/Rust/Zig/Flutter/Go/flatc/gomobile…） |
| 构建 | `zig → idl → rust → native → ts → go → gradle → flutter → verify` **全绿** |
| 产物 | `libyieyu_core.so`(8.4MB) + 3 个 Rust `.so` + `libyieyu_mem.a` + `cleaner.mjs`(60.7KB) + **APK 57.4MB** |
| 装机 | ✅ 能启动、UI 正常、原生核心加载成功 |
| 符号治理 | `exports.map` 白名单 + `-fvisibility=hidden` + `visibility("default")` 显式导出 |
| 换库 | **JUCE → SpeexDSP**（BSD-3，~300KB），删掉 5MB 引擎 + AGPL 传染 |

---

## 二、构建链 12 步（现在的真实状态）

| # | 步 | 产物 | 状态 |
|---|---|---|---|
| 0 | doctor | — | ✅ |
| 1 | idl | `out/gen/{cpp,dart,rust,go}` | ✅ |
| 2 | zig | `libyieyu_mem.a` | ✅ |
| 3 | rust | `libyieyu_{gesture,license,store}.so` | ✅ |
| 4 | ts | `out/ts/cleaner.mjs` 60.7KB | ✅ |
| 5 | wasm | `fx_tremolo.wasm` | ✅ |
| 6 | lua | 沙箱脚本白名单校验 | ✅ |
| 7 | go | `search.aar`（gomobile） | ✅ |
| 8 | native | `libyieyu_core.so` | ✅ |
| 9 | 符号自检 | 8 个 `Yieyu_*` 必须都在 | ✅（现在是硬失败，不许静默通过） |
| 10 | gradle | `platform-service.aar` | ✅ |
| 11 | flutter | `YieyuUltimate-arm64.apk` 57.4MB | ✅ + 装机验证 |

---

## 三、踩坑台账（22 条，全部真报错 + 真修法）

### A. 交付链路

| # | 报错 | 根因 | 修法 |
|---|---|---|---|
| 1 | `exit 126` | API 推 blob 丢可执行位 | `.sh/.mjs` 用 mode `100755` + CI `chmod +x` 兜底 |
| 2 | 两个 run 互相取消 | `concurrency.cancel-in-progress` + push/dispatch 抢组 | 一次只触发一个；别边推边 dispatch |
| 3 | artifact 下载 401 / 403 | `archive_download_url` 是 API 端点要 token，跳到 Azure 后又不能带 token | 按 host 分流：`api.github.com` 用 IP+token，Azure 只带 UA |
| 4 | `read ECONNABORTED`（30MB 处） | 移动网络长时间大包被掐 | Range 断点续传 + 重试（脚本 `fetch-apk.js`） |

### B. 各语言编译

| # | 报错 | 根因 | 修法 |
|---|---|---|---|
| 5 | `unknown root type: Cmd` | flatc 要求 `root_type` 在被引用 table **之后** | 移到文件末尾 |
| 6 | `invalid option: -Doptimize` | Zig 0.13：`standardOptimizeOption` 给了 `preferred_optimize_mode` 就不注册该选项 | 去掉 preferred |
| 7 | `manifest path rust/Cargo.toml does not exist` | cwd 已是 `rust/`，路径重复 | 用相对名 |
| 8 | `failed to parse manifest ... idna_adapter` | 钉 rustc 1.79，sqlx 依赖链要 ≥1.81 | toolchain 改 `stable` |
| 9 | `std::initializer_list was not found` | `for (auto* p : {a,b})` 需要显式头 | 补 `<initializer_list>` |
| 10 | `invalid operand for instruction` | AArch64 `LDP` 不支持寄存器偏移 `[x0,x4]` | 先 `add x6,x0,x4` 再 `ldp ...,[x6]` |
| 11 | `version script ... 'Yieyu_Free' not defined` | 白名单列了未实现符号 | 实现 `Yieyu_Free` |
| 12 | `Could not resolve "zod"` | esbuild 入口在 `out/`，向上找不到 `ts/node_modules` | 传 `NODE_PATH` |
| 13 | `gomobile bind requires golang.org/x/mobile` | 依赖图里没有 | 绑前 `go get golang.org/x/mobile` |
| 14 | `use of internal package ... not allowed` | gomobile 生成代码在临时模块，import 不了 `internal/` | `go/internal/bind` → **`go/bind`** |
| 15 | `elementAt` / `getter 'value'` | Dart FFI 的 `Array<Float>` 用下标且直接给 `double` | `eqDb[i]` |
| 16 | `Incompatible types 'String?' and 'Int'` | 我把 Intent action（String）和 native 码（Int）混成一个常量 | 拆成 `ACTION_*`（String）与 `CODE_*`（Int） |
| 17 | `friendPathsSet$kotlin_gradle_plugin_common` | 缺 `android.useAndroidX=true` | 加 `android/gradle.properties` |

### D. 播放链路（2026-11-13 打通 13→15 时踩到的）

| # | 报错 | 根因 | 修法 |
|---|---|---|---|
| 18 | 装机后任何网络都失败（音源直链放不出来、Lua 的 `http_get` 返回空） | `flutter create` 只在 **debug/profile** 壳子里加 `INTERNET`，release 的 main manifest 没有 | `app/patch_manifest.mjs` 注入 `INTERNET` + `ACCESS_NETWORK_STATE` |
| 19 | 装机没声音、日志里 `AAudio 全部打开失败` | 真机上 `AAUDIO_SHARING_MODE_EXCLUSIVE` 常被拒（非系统 App） | 独占+burst → 独占 → 共享+burst → 共享 **四级降级**，全失败才算失败 |
| 20 | 出来的声音是噪声/爆音 | `AMediaCodec` 输出可能是 16bit 也可能是 float，且**只有收到 format-changed 后** `pcm-encoding` 才可信 | 默认按 16bit 处理，收到 format-changed 再改；mono 复制成两路，>2 声道只取前两路 |

### E. Dart 侧（写测试音时踩的）

| # | 报错 | 根因 | 修法 |
|---|---|---|---|
| 21 | `The argument type 'bool' can't be assigned to the parameter type 'Endian'` | `ByteData.setInt16(off, val, endian)` 第三个参数是 `Endian`，不是"要不要小端"的 bool | 传 `Endian.little`（`dart:typed_data` 自带） |
| 22 | `'AMEDIAFORMAT_KEY_PCM_ENCODING' is unavailable: introduced in Android 28` | NDK 给这个 key 加了 `__INTRODUCED_IN(28)`，而 minSdk=26 | 用自己的字面量常量 `"pcm-encoding"`（key 的字符串值自 API 21 起就稳定） |

### B2. 脚本热更链路（2026-11-13 晚补）

| # | 报错/现象 | 根因 | 修法 |
|---|---|---|---|
| 20 | 导入 .lua 一律"导入失败" | `strstr(data,...)` 扫的是**没有 NUL 结尾**的裸字节，越界读 → 误判黑名单或崩 | 先 memcpy 到 len+1 的缓冲并补 `\0` 再扫 |
| 21 | 脚本里 `http_post` 是 nil | 沙箱只注册了 `http_get` | 注册 `http_get/http_post/request/fetch/http.*` 五个别名指向同一出口 |
| 22 | 导入成功后仍搜不到歌 | LuaJIT 没编进包（OFFLINE），且 `http_get` 背后没有 curl | CI 自动编 LuaJIT（失败降级）+ 明示 HTTP 要等 P2 接 libcurl |

| 23 | `'luaL_tolstring' was not declared` | LuaJIT 是 **Lua 5.1**，没有 5.2+ 的 `luaL_tolstring` | 改用 `lua_tostring` 逐参数取 |
| 24 | LuaJIT 编不出来时整轮构建失败 | 克隆/编译不可控 | 做成**非阻塞**：编不出来就走 stub，导入/校验照常 |

| 25 | 脚本 `http_get` 永远返回空 | 沙箱背后没有任何 HTTP 实现 | 新增 `go/fetch` → `libyieyu_fetch.so`（Go c-shared，自带 TLS），C++ 侧 `extern "C"` 直调；**不用 curl 是为了避开 OpenSSL 交叉编译** |

| 26 | 脚本里 `json` 是 nil | 沙箱从来没提供 JSON 库 | 自研 ~200 行 `json.decode/encode`（Lua 5.1 兼容，禁 `lua_absindex/lua_rawlen`） |
| 27 | `strtod` 报未声明 | 缺 `<cstdlib>` | 补齐 |

### C. 装机运行期（最值钱的两条）

| # | 报错 | 根因 | 修法 |
|---|---|---|---|
| 18 | `dlopen failed: library "libc++_shared.so" not found` | `ANDROID_STL=c++_shared` 但 gradle **不会**自动把它打进 APK | `build-app.sh` 从 NDK sysroot 拷进 `jniLibs/arm64-v8a/` |
| 19 | `undefined symbol: Yieyu_Init` | `-fvisibility=hidden` 把符号设成 hidden，**version script 提不起来** | 头文件加 `YIEYU_API __attribute__((visibility("default")))` |

> 这两条的共同教训：**链接成功 ≠ 能跑**。CI 的 `nm` 自检必须硬失败，
> 否则"编译全绿、装机全黑"会反复发生。

---

## 四、预判问题（还没炸，但迟早炸）

### 运行期
1. **16KB page 对齐**（Android 15+）：native 侧已加 `max-page-size=16384`，
   Rust 侧 `.cargo/config.toml` 也有，但**必须逐 so 用 `llvm-readelf -l` 验证**。
2. **R8 改 JNI 方法名**：`consumer-rules.pro` / `proguard-rules.pro` **还没写**，
   release 包一旦开混淆，`nativePing/nativeAction` 会被改名 → 通知栏按钮失效。
3. **`libyieyu_license.so` 没被 dlopen**：目前只是打进包，`Engine::Boot` 里验签是常量 `true`。
   接真验签时要 `dlopen` 一次并常驻句柄。
4. **AAudio 在模拟器上没有低延迟设备**：`AAUDIO_PERFORMANCE_MODE_LOW_LATENCY` 会静默降级。
5. **SQLite 双写**：`rust/store-sqlx` 必须保持 `query_only=1`，任何 INSERT 都会锁库 → 音频卡顿。

### 构建期
6. **Gradle 9 vs AGP 8.5.2**：runner 自带 gradle 是 9.x，AGP 8.5 只保证 8.x。
   现在能过，但 runner 一升级就会硬失败 → **尽快用 `gradle wrapper` 钉 8.7**。
7. **私有仓库分钟数**：一次全链路 ~18 分钟。建议日常用 `workflow_dispatch` 手动触发。
8. **`flutter create` 每次 CI 现场生成安卓壳**：没入库（刻意）。若哪天 Flutter 版本变了，
   壳子的 gradle 配置会跟着变 → 建议 P1 把壳子入库并锁死。

### 架构期
9. **模块 11（O-LLVM）套不上 NDK r27**：建议降级为"符号剥离 + 段校验 + R8"。
10. **模块 8（WasmEdge）Android 禁 JIT**：只能 interpreter，插件预算 32~64 采样/块。
11. **模块 18（ONNX）体积**：ORT arm64 ≈12MB，首包会到 70MB+。

---

## 五、还没写的东西

| 项 | 状态 |
|---|---|
| `consumer-rules.pro` / `proguard-rules.pro` | ❌ 未创建（开混淆前必须补） |
| `third_party/` 真实二进制（FFmpeg/SpeexDSP/Eigen/LuaJIT/WasmEdge/curl/SQLite/ORT） | ❌ 只有说明，空实现能编但不发声 |
| `tools/fetch-deps.mjs`（唯一下载入口 + sha256） | ❌ 未写 |
| 极速构建（五层缓存 + 模块路径过滤 + 补丁包 + 脚本热更） | ✅ **已上**（详见 `docs/BUILD.md` §8） |
| `gradle wrapper`（钉 Gradle 8.7） | ❌ 未加 |
| Rust 单测接入 CI（`cargo test`） | ❌ 本地有测试，CI 没跑 |
| Rive `.riv` 资源 | ❌ 未做（有降级绘制） |
| 真实播放链路（解码 → DSP → AAudio 出声） | ◐ **13→15 已通**（平台解码 + AAudio 出声，见 HANDOFF §2.6）；[14] 重采样 / [17] DSP 还没接 |

---

## 六、下次开工（按顺序，别跳）

1. `./build.sh doctor` 看一眼；直接 `workflow_dispatch` 一次确认还绿。
2. **P2 第一件事：只钉两个小依赖** —— SQLite（3.46 单文件）+ SpeexDSP（1.2.1）。
   写 `tools/fetch-deps.mjs`，下完校验 sha256 放进 `third_party/`。
3. ~~打通 9→13→15~~ ✅ **2026-11-13 已用 13→15 打通**（`demux_platform.c` + `aaudio_sink.c`，`player.cpp` 调度）。
   下一步：真机确认出声，再叠 [14] 重采样与 [17] DSP。
4. 出声之后再叠 17（EQ31 → 胆机 → 黑胶 LPF），每加一级都用真机听 + 看 underrun 计数。
5. FFmpeg 最后上（预编译包最容易踩 STL/ABI）。
6. 每次改动都要过：`doctor` → build → **`nm` 符号自检** → 装机 → 日志里看 `[15] AAudio 起播`。

---

## 七、许可证

自己折腾、不分发 → 焦虑归零。
JUCE 已换 SpeexDSP（BSD-3）。FFmpeg 仍建议不带 `--enable-gpl`（习惯问题，不影响自用）。
