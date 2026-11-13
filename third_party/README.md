# third_party —— 空目录（这是刻意的）

本工程**不引任何第三方二进制**：FFmpeg、libcurl、SQLite、Eigen、SpeexDSP、WasmEdge、
ONNX Runtime 全部不用。需要的功能都在树内自己实现，理由只有一条：
手机本机编不了、CI 每次都要下载上 GB 交叉编译产物、版本漂移还会把 `DT_NEEDED` 写坏
（历史上就踩过"NEEDED 里出现绝对路径 → 装机 dlopen 必炸"）。

| 原本要引的库 | 现在的实现 | 位置 |
|---|---|---|
| FFmpeg | NDK `mediandk`（系统解码器）+ 自研 DSF/DFF 解码 | `core/src/audio/demux_platform.c`、`dsd.cpp` |
| libcurl | 自研 socket HTTP/1.1 客户端（http），https 走 Go 的 TLS 桥 | `core/src/net/stream_http.c` |
| SQLite | 自研追加日志 KV（CRC + 断电截断 + 压实） | `core/src/store/kv_store.cpp` |
| SQLx | 自研 tokio 只读库（同一文件格式） | `rust/store-read/` |
| Eigen | 自研多相 FIR（DSD 抽取 + 重采样） | `core/src/audio/resampler.cpp`、`dsd.cpp` |
| SpeexDSP | 同上（有理数比例多相重采样，窗函数 sinc） | `core/src/audio/resampler.cpp` |
| WasmEdge | 自研 wasm MVP 解释器 + 自研插件生成器 | `core/src/wasm/wasm_interp.cpp`、`tools/mkwasm.mjs` |
| ONNX Runtime | 自研离线人声消除（STFT 中置抑制），核系数由 Python 生成 | `core/src/ai/vocals.cpp`、`python/vocal_remover/design.py` |
| Obfuscator-LLVM | 链接后处理：符号重命名 + strip + 自校验摘要 | `tools/obfuscate.mjs`、`core/src/common/integrity.cpp` |

**这里永远只放"怎么拿"的说明，不放代码、不放二进制。**
