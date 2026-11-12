# 纯血安卓 24 大技术巅峰架构表 · 权威版

> 本文件是**架构真源**。序号 1~24 与老板下发的表**一一对应，不得重排、不得合并**。
> 每一行只干一件事（"绝对不跨界"），跨界即视为架构违规，review 直接打回。
>
> 状态：**骨架已落盘，尚未编译**（本机无 NDK/Rust/Go/Flutter，构建全部走 CI 或桌面机）。

---

## 0. 七条铁律（谁违反谁负责）

1. **一个模块只干表里写的那一件事**。EQ 曲线不许在 Dart 里算，卡密验签不许落在 Kotlin。
2. **音频线程零 JNI、零 Dart、零分配**。`AAudio` 回调里只允许：读环、算、写环。
3. **跨语言只走两条路**：控制面 `JSB (FlatBuffers)`，数据面 `共享内存环 + FlatBuffers 头`。
   不许出现"用 JSON 传 PCM"这种事故。
4. **同一个 SQLite 文件只允许一个写者**。写者 = M20 (C++)；M19 (Rust SQLx) 只读 + 写元数据表，
   且必须 `journal_mode=WAL` + `busy_timeout=5000`，违反即锁库。
5. **卡密/验签只有一份密钥**，在 M10 (ring) 里；Kotlin、Dart、Lua、Wasm 一律不许持有。
6. **第三方脚本/插件运行在 M7 或 M8 的沙箱里**，不许碰文件系统全盘、不许 `dlopen`、不许起线程池。
7. **M22 Kotlin 总量封顶 400 行**（监控点：`wc -l android/**/*.kt`）。超了就说明有业务漏进系统层。

---

## 1. 24 模块总表

| # | 技术栈 | 最牛逼的库/工具 | 只干这 1 件事 | 物理路径 | 构建单元 | 产物 |
|---|---|---|---|---|---|---|
| 1 | Dart (Flutter) | Impeller | 宏观界面排版：页面跳转、列表框架 | `app/lib/shell/` | `flutter build apk` | `app-arm64-v8a-release.apk` |
| 2 | C++ | Rive Runtime | 微操矢量动画：31 段 EQ 曲线跳动、播放键呼吸灯 | `app/lib/animation/` → `rive_flutter`(内含 C++ runtime) | Flutter 插件 | `assets/rive/*.riv` |
| 3 | Rust | Tokio | UI 层高并发手势捕捉：防误触、长按震动判定 | `rust/gesture-tokio/` | `cargo build` | `libyieyu_gesture.so` |
| 4 | Zig | Zig CC | 跨平台底层编译 + 内存分配器 | `zig/` | `zig build` | `libyieyu_mem.a` |
| 5 | Go | Goroutine | 业务层高并发搜歌：同时打网易/QQ/酷狗 | `go/` | `gomobile bind` | `search.aar` |
| 6 | TypeScript | Zod / Axios | JSON 解析与数据清洗 | `ts/` | `tsc` + `esbuild` | `dist/cleaner.mjs` |
| 7 | Lua | LuaJIT | 第三方音源脚本沙箱（落雪源） | `core/src/script/` + `lua/sources/` | CMake 静态链 | `libyieyu_core.so` 内 |
| 8 | WebAssembly | WasmEdge | 跨端热更新插件：下发的音效包 | `core/src/wasm/` + `wasm/` | CMake + `cargo wasm32-wasip1` | `*.wasm` + `libwasmedge` |
| 9 | C | libcurl | 最底层 TCP/Socket：拉音频流 | `core/src/net/stream_curl.c` | CMake 静态链 | `libyieyu_core.so` 内 |
| 10 | Rust | ring | 密码学级验签：卡密、防抓包 | `rust/license-ring/` | `cargo build` | `libyieyu_license.so` |
| 11 | C++ | Obfuscator-LLVM | 机器码混淆与防篡改 | `core/cmake/ObfuscatorLLVM.cmake` + `tools/ollvm/` | 自定义 NDK 工具链 | 混淆后的 `.so` |
| 12 | Assembly | ARM64 NEON | 核心加密比对指令（微秒级硬指令） | `asm/neon_ct_compare.S` | CMake ASM | `libyieyu_core.so` 内 |
| 13 | C | FFmpeg | 解封装与解码：FLAC → PCM | `core/src/audio/demux_ffmpeg.c` + `third_party/ffmpeg/` | CMake 预编译库 | `libavformat/libavcodec.so` |
| 14 | C++ | **SpeexDSP** | 音频重采样：44.1k → 192k | `core/src/audio/resample_speex.cpp` | CMake（BSD-3，静态链） | `libyieyu_core.so` 内 |
| 15 | C | AAudio | 把 PCM 喂给手机声卡 | `core/src/audio/aaudio_sink.c` | CMake + `libaaudio` | `libyieyu_core.so` 内 |
| 16 | C++ | Eigen | DSD 转换的底层矩阵数学 | `core/src/audio/dsd_eigen.cpp` | header-only | 编进 core |
| 17 | C++ | 原生多线程池 | DSP 音效：31 段 EQ、胆机失真、黑胶 | `core/src/dsp/` | CMake 静态链 | `libyieyu_core.so` 内 |
| 18 | Python | ONNX Runtime | 本地 AI 人声消除推理 | `python/` + `core/src/ai/onnx_vocals.cpp` | CMake + ORT `.so` | `libonnxruntime.so` + `*.onnx` |
| 19 | Rust | SQLx | 本地数据库高速异步读写 | `rust/store-sqlx/` | `cargo build` | `libyieyu_store.so` |
| 20 | C++ | SQLite | 最底层磁盘持久化 | `core/src/store/sqlite_kv.cpp` | CMake | `libsqlite3.a` |
| 21 | C++ | FlatBuffers | 跨语言二进制打包（零拷贝） | `idl/*.fbs` + `core/src/ipc/` | `flatc` codegen | `gen/{cpp,dart,rust,go}/` |
| 22 | Kotlin | Service | 前台服务保活（通知栏） | `android/platform-service/` | Gradle 模块 | `platform-service.aar` |
| 23 | C/C++ | NDK ASensor | 绕过 Java 直接读陀螺仪，驱动 8D | `core/src/sensor/gyro_asensor.cpp` | CMake + `libandroid` | `libyieyu_core.so` 内 |
| 24 | C++ | 自研 JSB 引擎 | UI 与底层的数据桥梁 | `core/src/jsb/` | CMake | `libyieyu_core.so` 内 |

---

## 2. 完整工程目录结构

```
Yieyu-Ultimate-Player/
├── README.md                       架构速览 + 一图流
├── build.sh                        总控唯一入口（CI 与本地同一条命令）
├── Makefile                        build.sh 的糖（make apk / make rust / make doctor）
├── .gitignore
├── .github/workflows/ci.yml        手动触发：doctor + ts + go + cargo check
│
├── docs/
│   ├── ARCH_24.md                  ← 本文件（架构真源）
│   ├── DATAFLOW.md                 数据流向图（ASCII + Mermaid）
│   ├── BUILD.md                    跨语言构建总控方案
│   ├── CONTRACTS.md                跨语言契约（JSB ops / 环 / 表结构 / 命名规范）
│   └── ROADMAP.md                  分阶段落地（P0~P6）
│
├── tools/
│   ├── orchestrate.mjs             总控调度器（node 18+，零依赖，DAG+增量）
│   └── doctor.mjs                  工具链体检（缺什么喊什么）
│
├── app/                            ── 模块 1 / 2 / 3(调用) / 24(调用) ──
│   ├── pubspec.yaml
│   ├── analysis_options.yaml
│   └── lib/
│       ├── main.dart
│       ├── shell/                  【模块 1】宏观排版
│       │   ├── app_router.dart
│       │   └── home_page.dart
│       ├── animation/              【模块 2】微操动画
│       │   └── rive_stage.dart
│       ├── gestures/               【模块 3】Tokio 手势的 Dart 侧
│       │   └── gesture_bridge.dart
│       └── jsb/                    【模块 24】JSB 的 Dart 侧
│           ├── jsb_client.dart
│           └── jsb_ops.dart        ← 由 idl/jsb.fbs 生成，勿手改
│
├── core/                           ── 模块 5?no: 7/8/9/10(调用)/12/13/14/15/16/17/18/20/21/23/24 ──
│   ├── CMakeLists.txt              总 CMake（find_package 各语言产物）
│   ├── cmake/
│   │   ├── android-arm64.cmake     NDK 工具链 + 产物落位约定
│   │   ├── FindFFmpeg.cmake
│   │   └── ObfuscatorLLVM.cmake    【模块 11】混淆钩子
│   ├── src/
│   │   ├── jsb/                    【模块 24】桥
│   │   │   ├── jsb.h
│   │   │   └── jsb.cpp
│   │   ├── pipeline/
│   │   │   └── engine.cpp          总指挥：装/卸各模块，唯一状态机
│   │   ├── audio/
│   │   │   ├── demux_ffmpeg.c      【模块 13】
│   │   │   ├── resample_speex.cpp  【模块 14】
│   │   │   ├── aaudio_sink.c       【模块 15】
│   │   │   └── dsd_eigen.cpp       【模块 16】
│   │   ├── dsp/                    【模块 17】
│   │   │   ├── thread_pool.h
│   │   │   ├── eq31.cpp
│   │   │   ├── tube_vinyl.cpp
│   │   │   └── hrtf_8d.cpp         8D 的空间化（吃模块 23 的 yaw/pitch）
│   │   ├── sensor/
│   │   │   └── gyro_asensor.cpp    【模块 23】
│   │   ├── net/
│   │   │   └── stream_curl.c       【模块 9】
│   │   ├── store/
│   │   │   └── sqlite_kv.cpp       【模块 20】
│   │   ├── ipc/
│   │   │   └── flatbuf_codec.cpp   【模块 21】
│   │   ├── script/
│   │   │   └── lua_sandbox.cpp     【模块 7】
│   │   ├── wasm/
│   │   │   └── host_wasmedge.cpp   【模块 8】
│   │   └── ai/
│   │       └── onnx_vocals.cpp     【模块 18】
│   └── tests/
│       └── smoke_test.cpp
│
├── rust/                           ── 模块 3 / 10 / 19 ──
│   ├── Cargo.toml                  workspace
│   ├── rust-toolchain.toml
│   ├── .cargo/config.toml          aarch64-linux-android 链接器
│   ├── gesture-tokio/              【模块 3】
│   │   ├── Cargo.toml
│   │   └── src/lib.rs
│   ├── license-ring/               【模块 10】
│   │   ├── Cargo.toml
│   │   └── src/lib.rs
│   └── store-sqlx/                 【模块 19】
│       ├── Cargo.toml
│       └── src/lib.rs
│
├── asm/                            ── 模块 12 ──
│   ├── neon_ct_compare.S
│   └── README.md
│
├── go/                             ── 模块 5 ──
│   ├── go.mod
│   ├── cmd/searchd/main.go
│   └── internal/
│       ├── fanout/fanout.go        并发扇出 + 合并
│       └── providers/provider.go   源站适配接口
│
├── ts/                             ── 模块 6 ──
│   ├── package.json
│   ├── tsconfig.json
│   └── src/
│       ├── schema/song.ts          Zod：唯一真源
│       ├── net/client.ts           Axios：只发请求，不解析
│       └── clean/index.ts          洗数据 → 干净状态
│
├── lua/                            ── 模块 7 ──
│   ├── README.md                   沙箱 API 白名单
│   └── sources/example_source.lua
│
├── wasm/                           ── 模块 8 ──
│   ├── README.md
│   └── fx_tremolo/                 示例热更新音效包
│       ├── Cargo.toml
│       └── src/lib.rs
│
├── zig/                            ── 模块 4 ──
│   ├── build.zig
│   └── src/alloc/pool.zig
│
├── python/                         ── 模块 18 ──
│   ├── requirements.txt
│   ├── README.md
│   └── vocal_remover/
│       └── infer.py                只做推理；导出 ONNX 也在这一步
│
├── idl/                            ── 模块 21 ──
│   ├── jsb.fbs                     控制面：JSB 请求/事件
│   └── frames.fbs                  数据面：共享内存环头
│
├── android/                        ── 模块 22（全场唯一 Kotlin） ──
│   ├── settings.gradle.kts
│   └── platform-service/
│       ├── build.gradle.kts
│       └── src/main/
│           ├── AndroidManifest.xml
│           └── kotlin/com/yieyu/ultimate/service/
│               └── PlaybackService.kt
│
└── third_party/                     ── 只放"怎么拿"，不放代码 ──
    └── README.md                   FFmpeg/JUCE/Eigen/SQLite/ORT/WasmEdge/LuaJIT 版本与许可证
```

---

## 3. 运行时产物落位（谁生成、谁消费）

| 产物 | 生产者 | 落位 | 消费者 |
|---|---|---|---|
| `libyieyu_mem.a` | M4 zig | `out/zig/arm64/` | core (链接) |
| `libyieyu_gesture.so` | M3 cargo | `out/rust/arm64/` | Flutter (`dart:ffi`) |
| `libyieyu_license.so` | M10 cargo | `out/rust/arm64/` | core (`dlopen` 一次，句柄常驻) |
| `libyieyu_store.so` | M19 cargo | `out/rust/arm64/` | core |
| `search.aar` | M5 gomobile | `out/go/` | Android (极薄 Kotlin 之外直接 JNI? 否 → JSB) |
| `dist/cleaner.mjs` | M6 tsc | `out/ts/` | M8 WasmEdge-JS guest 或 Go 侧兜底 |
| `libyieyu_core.so` | core CMake | `out/native/arm64/` | Flutter + Kotlin Service |
| `*.wasm` | `wasm/*` cargo | `assets/plugins/` | M8 宿主 |
| `platform-service.aar` | M22 gradle | Gradle 本地模块 | app |

> **链接顺序铁律**：`core` 是唯一的"汇聚点"。Rust/Zig/FFmpeg 全部被 core 静态或动态链接，
> **Flutter 只直接 FFI 三个 so**：`core`、`gesture`、`store`。不许 App 直接链 `license`。

---

## 4. 真话与风险（写在前面，免得做到一半翻车）

| 项 | 现实约束 | 对策 |
|---|---|---|
| **M11 Obfuscator-LLVM** | NDK r23 起弃用 GCC 前端；O-LLVM 是 LLVM 分支，无法直接套 NDK r27 的 clang | 走 LLVM Pass Plugin（`-fpass-plugin`）自研混淆 pass，或自建 O-LLVM + 自编 libc++；**P4 再做** |
| **M12 NEON 手写汇编** | 手写比对必须**常数时间**，且要防编译器重排 | 汇编里只用寄存器 + `cbz` 分支，禁止早退；上机做 dudect 泄漏测试 |
| **M18 Python + ONNX** | 手机上跑 CPython 体积 +120MB 且 GIL 拖累实时性 | **Python 只做训练/导出**；运行时走 `onnxruntime` C++ API（`core/src/ai/onnx_vocals.cpp`）；真要 Py 就 Chaquopy 且仅臂 64 |
| **M8 WasmEdge** | Android 上 AOT 受限（JIT 被 W^X 拦），需关 JIT 走 interpreter 或预编译 AOT | 插件只做**参数级音效**（32~64 采样一块），interpreter 够用；预编译 AOT 放首启解压 |
| **M7 LuaJIT** | 进程内沙箱死循环只能软掐（指令计数 hook） | 双模：默认进程内 hook，`--hard-kill` 模式走独立进程，超时 `SIGKILL` |
| **M4 Zig CC** | 用它替 NDK clang 编整个 NDK 生态会踩 STL/ABI 坑 | 只编"零依赖"模块（分配器/算法核），**不碰** libc++ 边界 |
| **M3 Tokio 抓手势** | 触摸事件源头在 Flutter（Impeller 之前就分发了） | 事件经 `JSB` 单向下行给 Tokio 任务组，判定结果走 `gesture` so 回 Dart；**Tokio 不碰 UI** |
| **M19+M20 双写 SQLite** | 双写者必锁库 | WAL + 写者唯一（M20）；M19 只读 + `PRAGMA query_only` |
| **M1 Flutter 体积** | 首包 +18MB，冷启 +300ms | 分 ABI 出包（arm64 only 起步，和 Core-Player 一致） |

---

## 5. 版本锚点

| 组件 | 版本 | 备注 |
|---|---|---|
| Flutter / Dart | 3.24.x / 3.5 | Impeller 默认即开（Android Vulkan） |
| NDK | r27c | `ANDROID_NDK` |
| minSdk / targetSdk | 26 / 34 | **26 是 AAudio 的硬门槛** |
| Rust | 1.79 stable | `edition = 2021`，targets: `aarch64-linux-android` |
| Go | 1.22 | gomobile + gomobile bind |
| Zig | 0.13.0 | `-target aarch64-linux-android` |
| C++ | C++20 | `-fno-exceptions -fno-rtti`（音频核） |
