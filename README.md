# Yieyu-Ultimate-Player

> **纯血安卓 24 大技术巅峰架构**。一个音源、播放、音效、8D、卡密全链路的 Android 播放器。
> 24 个模块，每个只干一件事，绝不允许跨界。
> **零第三方二进制**：需要的功能全部自研或在树内生成（见 [`third_party/README.md`](third_party/README.md)）。

```
Flutter ─ Rust(Tokio, ring) + Zig + Go(TLS) + TypeScript(构建期) + LuaJIT ─ NDK mediandk/AAudio/ASensor ─ 自研 C/C++ 内核
```

| | |
|---|---|
| 架构真源 | [`docs/ARCH_24.md`](docs/ARCH_24.md)（24 模块表 + 全量目录 + 风险） |
| 数据流图 | [`docs/DATAFLOW.md`](docs/DATAFLOW.md) |
| 构建总控 | [`docs/BUILD.md`](docs/BUILD.md) |
| 跨语言契约 | [`docs/CONTRACTS.md`](docs/CONTRACTS.md)（JSB 帧格式 / ABI 版本 / KV 记录格式） |
| 落地节奏 | [`docs/ROADMAP.md`](docs/ROADMAP.md) |
| 交接 | [`HANDOFF.md`](HANDOFF.md) |

---

## 24 模块一览（"库"这一列是**现在真正在跑的**，不是想引的）

| # | 技术栈 | 用的东西 | 只干这 1 件事 | 代码位置 |
|---|---|---|---|---|
| 1 | Dart (Flutter) | Impeller | 宏观界面：页面跳转、列表、设置、自检页 | `app/lib/shell`、`app/lib/diag` |
| 2 | 自研矢量动画 | 弹簧状态机 + CustomPainter | 31 段 EQ 曲线跳动、播放键呼吸灯 | `app/lib/animation/vector_stage.dart` |
| 3 | Rust + Tokio | 多线程 runtime | 手势捕捉：滑动切歌、长按换 EQ | `rust/gesture-tokio` |
| 4 | Zig | 自研定长池 | JSB 命令帧分配（不碰 malloc） | `zig/` |
| 5 | Go | net/http + TLS1.3 | 并发搜歌 + 取直链 | `go/fetch` |
| 6 | TypeScript | Zod（构建期） | JSON 清洗 + 生成 Dart 歌曲模型 | `ts/`、`tools/gen-schema.mjs` |
| 7 | Lua (LuaJIT) | 沙箱 + 指令计数 | 第三方音源脚本（随包 `wy.lua`） | `core/src/script`、`lua/sources` |
| 8 | WebAssembly | 自研 MVP 解释器 | 热更新音效插件（不下发 APK） | `core/src/wasm`、`tools/mkwasm.mjs` |
| 9 | C | BSD socket | HTTP/1.1 取流（chunked/302/大小上限） | `core/src/net/stream_http.c` |
| 10 | Rust + ring | Ed25519 | 卡密验签（唯一密钥点） | `rust/license-ring` |
| 11 | 自研防篡改 | 符号重命名 + strip + sha256 | 机器码收紧与完整性自校验 | `tools/obfuscate.mjs`、`core/src/common/integrity.cpp` |
| 12 | ARM64 NEON | 手写汇编 | 常数时间摘要比对 | `asm/neon_ct_compare.S` |
| 13 | C | NDK mediandk | 解封装解码（mp3/flac/m4a/aac/wav…） | `core/src/audio/demux_platform.c` |
| 14 | C++ | 自研多相 FIR | 重采样 22.05k~192k 任意互转 | `core/src/audio/resampler.cpp` |
| 15 | C | AAudio | PCM 喂声卡（独占→共享四级降级） | `core/src/audio/aaudio_sink.c` |
| 16 | C++ | 自研 DSF/DFF 解码 | DSD → PCM（FIR÷8 + 模块14） | `core/src/audio/dsd.cpp` |
| 17 | C++ | 自研 DSP | 31 段 EQ / 胆机 / 黑胶 / 8D | `core/src/dsp` |
| 18 | C++ + Python | 自研 STFT 中置抑制 | 离线人声消除（生成伴奏），核系数 Python 生成 | `core/src/ai`、`python/vocal_remover` |
| 19 | Rust + Tokio | 自研只读库 | 本地库异步只读（与 20 同一文件） | `rust/store-read` |
| 20 | C++ | 自研追加日志 | 唯一写者：会话/进度/预设（CRC + 断电截断 + 压实） | `core/src/store` |
| 21 | 自研二进制契约 | 40 字节帧头 | 跨语言零拷贝打包，`idl/*.fbs` 为真源 | `core/src/ipc`、`idl/` |
| 22 | Kotlin | Service | 前台服务保活（通知栏播控，唯一 JNI 面） | `android/platform-service` |
| 23 | C/C++ | NDK ASensor | 直读陀螺仪/旋转矢量 → 8D | `core/src/sensor` |
| 24 | C++ | 自研 JSB | UI ↔ 底层唯一天桥（dart:ffi，无 JNI） | `core/src/jsb` |

---

## 30 秒看懂结构

```
app/      Flutter 顶层（模块 1/2）+ FFI（3/24）
core/     C++ 汇聚层 → libyieyu_core.so（7/8/9/12~18/20/21/23/24）
rust/     手势 / 验签 / 只读库（3/10/19）
go/       搜歌 + 取直链（5），C ABI 出口 libyieyu_fetch.so
ts/       构建期 schema → Dart 模型（6）
zig/      分配器（4）→ libyieyu_mem.a
asm/      NEON 常数时间比对（12）
lua/      随包音源脚本（7）
python/   人声消除核系数生成（18）
android/  唯一 Kotlin（22，预算 400 行）
idl/      跨语言契约（21）
```

---

## 一条命令

```bash
./build.sh doctor          # 先看缺什么工具链
./build.sh                 # 全量（缺工具的步骤 SKIP，不会瞎 FAIL）
./build.sh native flutter  # 只跑指定步骤
node tools/verify-idl.mjs  # 契约真源校验（CI 里在 idl 步骤自动跑）
node tools/check-ai-kernel.mjs   # 模块18 核系数与 Python 生成器是否一致
```

产物全部落在 `out/`，源码树只读（唯一例外：构建期生成 `app/lib/model/song.dart` 与 `core/src/ai/vocal_kernel.h`）。

---

## 三条铁律（违反必回退）

1. **音频线程零 JNI、零 Dart、零分配** —— AAudio 回调里只有"读环、算、写环"。
2. **同一个存储文件只有一个写者** —— 写者是模块 20；模块 19 只读。
3. **Kotlin 总量封顶 400 行** —— 超了就说明业务漏进系统层。

完整七条见 [`docs/ARCH_24.md`](docs/ARCH_24.md) §0。

---

## 现在到哪一步（不吹）

- ✅ 云端构建绿：Zig → Rust → idl（含契约校验）→ Go → C++ → TS/schema → Gradle → 混淆 → Flutter → verify
- ✅ 装机可跑：搜索 → 直链 → 解码 → 重采样 → DSP → AAudio 出声；Lua 脚本导入与执行；DSD 文件直读
- ✅ 24 个模块**都有真实调用点**：设置 →「模块自检」页里逐个能看到在跑
- ⚠️ 仍然是 arm64-only、无 iOS；FFmpeg/ONNX/WasmEdge 的"大而全"能力用的是自研等价实现
- 📋 踩坑台账在 [`docs/BLUEPRINT.md`](docs/BLUEPRINT.md)

## 许可证 / 红线

- 本仓库暂未选定开源许可证（**默认私有**）。
- 零第三方二进制 → 没有 AGPL/GPL 传染问题；Rust/npm 依赖的许可证见各自 `Cargo.toml` / `package.json`。
- 密钥只存在于模块 10；任何其它模块持有密钥副本都视为安全事故。
