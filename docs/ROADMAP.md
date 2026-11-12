# 落地节奏 · P0 → P6

> 每一阶段都能**单独发版、单独回滚**。不许出现"憋一个大版本"。

| 阶段 | 目标 | 交付物 | 验收 |
|---|---|---|---|
| **P0** ✅ | 骨架 | 24 模块目录、契约、构建总控、数据流图 | `./build.sh doctor` 有输出；CI `contract` 通过 |
| **P1** | 空实现能编出 `.so` | `YIEYU_OFFLINE_ASSETS=ON` 下 `libyieyu_core.so` + 三个 Rust `.so` + `libyieyu_mem.a` | `./build.sh zig idl rust native` 全绿；`out/verify/MANIFEST.md` 有 5 个产物 |
| **P2** | 真解码出声 | 拉齐 `third_party/`（FFmpeg/JUCE/curl/SQLite/FlatBuffers），打通 9→13→14→17→15 | 44.1k FLAC 本地文件出声，AAudio 回调 <1.2ms/块（ARM64 真机） |
| **P3** | 音效与空间 | 31 段 EQ + 胆机 + 黑胶（含 16k LPF）+ 8D（ASensor 头追 + 自动旋转） | 头追延迟 <40ms；EQ 曲线跳动与 Rive 同步不掉帧 |
| **P4** | 安全与反破解 | O-LLVM pass + 符号剥离 + NEON 常数时间比对 + 卡密全链路 | dudect `|t|<4.5`；逆向拿到 `.so` 无有效符号 |
| **P5** | 联网与脚本 | Go 搜歌 AAR + TS Zod 清洗 + Lua 沙箱（落雪源）+ Wasm 插件热更新 | 3 源并发 <800ms 出结果；恶意脚本无法 `os.execute`；插件下发热更生效 |
| **P6** | AI 与打磨 | ONNX 人声消除（离线通道）+ Kotlin 通知栏完整态 + 分 ABI 出包 | KTV 伴奏整曲处理；前台服务 8h 不被杀 |

---

## 每阶段的"不许跳"清单

- **P2 之前不许碰模块 11**（混淆）：先让链路对了，混淆会毁栈回溯，调试成本翻倍。
- **P3 之前不许开 DSD**：DSD 需要 Eigen + FFmpeg 双通，先把 PCM 主链稳住。
- **P5 之前不许给 Lua 开 `http` 以外的能力**：脚本是最容易被社区改的面。
- **任何阶段都不许**：在 AAudio 回调里加一行日志（想 debug 就上环计数器）。

---

## 已知的四个硬风险（都写在 docs/ARCH_24.md §4）

1. **M11 O-LLVM** 套不上 NDK r27 → 走 LLVM Pass Plugin 或自建工具链（P4 才做，可能延期）
2. **M18 Python** 手机端不可用 → Python 只做训练/导出，运行时是 ORT C++
3. **M8 WasmEdge** Android 上 JIT 被 W^X 拦 → interpreter / 预编译 AOT
4. **M14 JUCE** 是 AGPLv3 → 闭源商业发行必须买授权（`third_party/README.md` 红字）

---

## 下一步（给接手的会话看）

```bash
./build.sh doctor                 # 看缺什么
./build.sh --dry-run              # 看完整执行计划（不编译）
./build.sh zig idl rust native    # 第一目标：让 .so 出来
```

P1 完成的标准很硬：`out/verify/MANIFEST.md` 里必须有
`libyieyu_core.so` / `libyieyu_gesture.so` / `libyieyu_license.so` / `libyieyu_store.so` / `libyieyu_mem.a` 五个。
