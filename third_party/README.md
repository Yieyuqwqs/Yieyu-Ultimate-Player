# third_party · 只放"怎么拿"，不放代码

二进制不进仓库（体积 + 许可证 + 供应链透明）。`./build.sh` 在 `YIEYU_OFFLINE_ASSETS=ON` 时走空实现；
要真编，按下面把依赖放到指定路径。

| 模块 | 依赖 | 版本锚点 | 落位 | 许可证 | 备注 |
|---|---|---|---|---|---|
| 13 | **FFmpeg** | n7.0.2 | `third_party/ffmpeg/<abi>/{include,lib}` | LGPL-2.1+（**不带 --enable-gpl**） | NDK r27 编出，必须 `c++_shared` |
| 14 | **JUCE** | 8.0.0 | `third_party/juce/`（`add_subdirectory`） | AGPLv3 / 商业 | ⚠️ **AGPL 会传染整个 App**，商业发行必须买 JUCE 授权 |
| 15 | AAudio | NDK 自带 | — | Apache-2.0 | minSdk 26 |
| 16 | **Eigen** | 3.4.0 | `third_party/eigen/Eigen/` | MPL2 | header-only |
| 17 | 无（自研） | — | — | — | 线程池 + 31 段 EQ + 胆机 + 黑胶 |
| 18 | **ONNX Runtime** | 1.19.2 (android arm64) | `third_party/onnxruntime/` | MIT | 只 arm64 |
| 7 | **LuaJIT** | 2.1-20240815 | `third_party/lua/` | MIT | arm64 无官方预编译，需自编 |
| 8 | **WasmEdge** | 0.13.5 | `third_party/wasmedge/` | Apache-2.0 | 关 JIT 编（Android W^X） |
| 9 | **libcurl** | 8.9.1 | `third_party/curl/` | MIT-like | SSL 接 BoringSSL 或关掉，二选一 |
| 20 | **SQLite** | 3.46.1 | `third_party/sqlite/` | Public Domain | 开 WAL |
| 21 | **FlatBuffers** | 24.3.25 | `third_party/flatbuffers/`（`include/` + `flatc`） | Apache-2.0 | 生成代码进 `out/gen` |
| 10/19 | crate 依赖 | 见 `rust/Cargo.toml` | — | ring: ISC/Apache-2.0 | — |
| 5 | Go stdlib | 1.22 | — | BSD-3 | 无第三方依赖 |

## 一键拉取

`tools/fetch-deps.mjs` 现在**故意没有**。骨架阶段先让"空实现能编过"，等契约稳了再钉死版本；
到那时它是唯一允许下载二进制的入口，每个文件带 sha256 校验。

## 许可证红线

- **JUCE 是 AGPLv3**：闭源商业发行 = 必须买 JUCE 商业授权。整张表里唯一影响"能不能卖"的依赖。
- **FFmpeg 别开 `--enable-gpl`**：开了整包 GPL，与闭源分发冲突。
- Eigen 是 MPL2（文件级弱传染），静态链进 `.so` 没问题，但必须保留声明。
