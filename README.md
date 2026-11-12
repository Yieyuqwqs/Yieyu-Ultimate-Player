# Yieyu-Ultimate-Player

> **纯血安卓 24 大技术巅峰架构**。一个音源、播放、音效、8D、卡密全链路的 Android 播放器。
> 24 个模块，每个只干一件事，绝不允许跨界。

```
Flutter+Rive ─ Go+TS+Lua ─ Rust(ring,Tokio)+C++ ─ NDK ASensor ─ FFmpeg+JUCE+SQLite+AAudio ─ 极少 Kotlin
```

| | |
|---|---|
| 架构真源 | [`docs/ARCH_24.md`](docs/ARCH_24.md)（24 模块表 + 全量目录 + 风险） |
| 数据流图 | [`docs/DATAFLOW.md`](docs/DATAFLOW.md)（8 张图，含 Mermaid） |
| 构建总控 | [`docs/BUILD.md`](docs/BUILD.md)（CMake + Cargo + Go + Zig + tsc + Gradle） |
| 跨语言契约 | [`docs/CONTRACTS.md`](docs/CONTRACTS.md)（JSB ops / 环头 / 表结构 / 版本规则） |
| 落地节奏 | [`docs/ROADMAP.md`](docs/ROADMAP.md)（P0 → P6，每阶段可单独发版） |

---

## 24 模块一览

| # | 技术栈 | 库 | 只干这 1 件事 |
|---|---|---|---|
| 1 | Dart (Flutter) | Impeller | 宏观界面排版：页面跳转、列表框架 |
| 2 | C++ | Rive Runtime | 微操矢量动画：EQ 曲线跳动、播放键呼吸灯 |
| 3 | Rust | Tokio | UI 层高并发手势捕捉：防误触、长按判定 |
| 4 | Zig | Zig CC | 跨平台底层编译 + 内存分配器 |
| 5 | Go | Goroutine | 业务层高并发搜歌（网易/QQ/酷狗同时打） |
| 6 | TypeScript | Zod / Axios | JSON 解析与数据清洗 |
| 7 | Lua | LuaJIT | 第三方音源脚本沙箱 |
| 8 | WebAssembly | WasmEdge | 跨端热更新插件（下发音效包） |
| 9 | C | libcurl | 最底层 TCP/Socket，拉音频流 |
| 10 | Rust | ring | 密码学级验签（卡密 / 防抓包） |
| 11 | C++ | Obfuscator-LLVM | 机器码混淆与防篡改 |
| 12 | Assembly | ARM64 NEON | 核心加密比对硬指令 |
| 13 | C | FFmpeg | 解封装解码（FLAC → PCM） |
| 14 | C++ | **SpeexDSP**（原 JUCE） | 音频重采样（44.1k → 192k） |
| 15 | C | AAudio | PCM 喂给声卡 |
| 16 | C++ | Eigen | DSD 转换的矩阵数学 |
| 17 | C++ | 原生多线程池 | DSP：31 段 EQ / 胆机 / 黑胶 |
| 18 | Python | ONNX Runtime | 本地 AI 人声消除 |
| 19 | Rust | SQLx | 本地库高速异步读写 |
| 20 | C++ | SQLite | 最底层磁盘持久化 |
| 21 | C++ | FlatBuffers | 跨语言二进制打包（零拷贝） |
| 22 | Kotlin | Service | 前台服务保活（通知栏） |
| 23 | C/C++ | NDK ASensor | 绕过 Java 直读陀螺仪 → 8D |
| 24 | C++ | 自研 JSB | UI 与底层的桥（dart:ffi，无 JNI） |

---

## 30 秒看懂结构

```
app/      Flutter 顶层（模块 1/2）+ 两个 ffi 绑定（模块 3/24）
core/     C++ 汇聚层（模块 7/8/9/12~18/20/21/23/24）→ libyieyu_core.so
rust/     Cargo workspace（模块 3 手势 / 10 验签 / 19 只读库）
go/       协程搜歌（模块 5），唯一出口 internal/bind
ts/       Zod 洗数据（模块 6）→ out/ts/cleaner.mjs
zig/      分配器（模块 4）→ libyieyu_mem.a
asm/      NEON 常数时间比对（模块 12）
lua/      音源脚本（模块 7 的沙箱内容）
wasm/     音效插件（模块 8 的 guest）
python/   模型训练/导出（模块 18 的离线侧）
android/  全场唯一 Kotlin（模块 22，预算 400 行）
idl/      跨语言契约（模块 21）
```

**谁汇聚谁**：一切静态/动态库最终都链进 `libyieyu_core.so`；
Flutter 只直接 FFI 三个 so（`core`、`gesture`、`store`）。

---

## 一条命令

```bash
./build.sh doctor          # 先看缺什么工具链
./build.sh                 # 全量（缺工具的步骤会 SKIP，不会瞎 FAIL）
./build.sh rust zig native # 只跑指定步骤（自动带依赖）
./build.sh clean native    # 只清某一步产物
```

产物全部落在 `out/`，源码树只读。`./build.sh verify` 出 `out/verify/MANIFEST.md`（sha256 + 体积）。

---

## 三条铁律（违反必回退）

1. **音频线程零 JNI、零 Dart、零分配** —— AAudio 回调里只有"读环、算、写环"。
2. **同一个 SQLite 文件只有一个写者** —— 写者是模块 20；模块 19 只读（`query_only=1`）。
3. **Kotlin 总量封顶 400 行** —— 超了就说明有业务漏进系统层（`./gradlew checkKotlinBudget` 会红）。

完整七条见 [`docs/ARCH_24.md`](docs/ARCH_24.md) §0。

---

## 现在到哪一步了（不吹）

- ✅ 24 模块目录、契约、构建总控、数据流图全部在位
- ⚠️ **还没编译过一次**：`third_party/` 只有说明，二进制未入库；
  `YIEYU_OFFLINE_ASSETS=ON` 会编出"空实现"版本，用来先验证骨架能过
- ⏭️ 下一步：`./build.sh doctor` → 补齐工具链 → `./build.sh zig idl native`（空实现）先出 `.so`

## 许可证 / 红线

- 本仓库暂未选定开源许可证（**默认私有**）。
- 依赖侧注意：**JUCE 是 AGPLv3**，闭源商业发行必须购买授权；FFmpeg 不许开 `--enable-gpl`。
- 密钥只存在于模块 10；任何其它模块持有密钥副本都视为安全事故。
