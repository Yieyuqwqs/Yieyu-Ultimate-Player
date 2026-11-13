# 跨语言构建总控方案（CMake + Cargo + Go Modules + Zig + tsc + Gradle）

> 唯一入口：`./build.sh [target...]`。CI 与本地**同一条命令**，不许有第二套脚本。
> 调度器 `tools/orchestrate.mjs`：零依赖 Node，DAG + 增量（按输入 mtime + 命令指纹）。

---

## 0. 一句话总控

```
build.sh  →  tools/orchestrate.mjs  →  { zig | asm↦cmake | cargo | gomobile | tsc | wasm | flatc | gradle | flutter }
                                  →  $OUT/{zig,rust,native,go,ts,wasm,gen,apk}/
```

**铁律：任何语言都不许把产物写进源码树**。源码树只读，产物只进 `$OUT`（默认 `./out`）。
这样"干净重编"= `rm -rf out && ./build.sh`，且 CI 缓存边界天然清晰。

---

## 1. 拓扑顺序（DAG，`orchestrate.mjs` 里同名声明）

```
                     ┌──────────────┐
                     │ 0. doctor    │  工具链体检（缺工具立刻报，别等 30 分钟后）
                     └──────┬───────┘
        ┌───────────┬───────┼────────┬───────────┬──────────┬─────────┐
        ▼           ▼       ▼        ▼           ▼          ▼         ▼
   ┌────────┐ ┌─────────┐┌──────┐┌───────┐┌──────────┐┌────────┐┌─────────┐
   │1 idl   │ │2 zig    ││3 rust││4 go   ││5 ts      ││6 wasm  ││7 lua    │
   │ flatc  │ │  mem.a  ││ 3 个 ││ search││ cleaner  ││ *.wasm ││ 资源拷贝 │
   │ codegen│ │         ││ .so  ││ .aar  ││ .mjs     ││        ││         │
   └───┬────┘ └────┬────┘└──┬───┘└───┬───┘└────┬─────┘└───┬────┘└────┬────┘
       │           │        │        │         │          │          │
       └───────────┴────────┴───┬────┴─────────┴──────────┴──────────┘
                                ▼
                      ┌───────────────────┐
                      │ 8. native (CMake) │  libyieyu_core.so  ← 汇聚点
                      │  NDK r27 + ninja  │  链: rust/*.so + zig/mem.a + ffmpeg + lua
                      └─────────┬─────────┘
                                ▼
                      ┌───────────────────┐
                      │ 9. gradle         │  platform-service.aar [22]
                      └─────────┬─────────┘
                                ▼
                      ┌───────────────────┐
                      │ 10. flutter apk    │  注入 out/ 全部产物 → app-arm64.apk
                      └─────────┬─────────┘
                                ▼
                      ┌───────────────────┐
                      │ 11. manifest+verify│ sha256 / 体积 / 符号表快照
                      └───────────────────┘
```

> **并行度**：1~7 相互独立，调度器按 `os.cpus()` 并起（默认 4）。
> **唯一串行点**：native 必须等 `zig + rust + idl`；flutter 必须等 `native + go + ts + wasm + lua + gradle`。

---

## 2. 每个语言怎么编（命令 + 关键参数）

### 2.1 CMake（模块 12/13/14/15/16/17/20/21/23/24/7/8/9/18）

```bash
cmake -S core -B $OUT/native -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=core/cmake/android-arm64.cmake \
  -DCMAKE_BUILD_TYPE=Release \
  -DYIEYU_OUT=$OUT \
  -DYIEYU_OFFLINE_ASSETS=ON          # 无网/无 third_party 时编"空实现"，让骨架先过
cmake --build $OUT/native -j
```

关键 CMake 决定（见 `core/CMakeLists.txt`）：

| 决定 | 值 | 理由 |
|---|---|---|
| `ANDROID_STL` | `c++_shared` | 与 SpeexDSP/FFmpeg 预编译库统一，避免两套 STL |
| `-fvisibility=hidden` | ON | 只导出 JSB 的 8 个符号，其余全藏（配合 [11] 混淆） |
| `-fno-exceptions -fno-rtti` | 音频核（dsp/audio） | 实时路径零异常开销 |
| `ANDROID_ARM_NEON` | TRUE | [12] 汇编与 Eigen 都要 |
| `CMAKE_CXX_STANDARD` | 20 | `std::atomic_ref` / `span` 用于环 |
| `aars` | 不参与 | 只在 gradle 侧打进去 |

### 2.2 Cargo（模块 3/10/19）

```bash
cargo build --manifest-path rust/Cargo.toml --release \
  --target aarch64-linux-android
# 产物统一由 .cargo/config.toml 落到 $OUT/rust/<target>/
```

- workspace 三个 crate 全部 `crate-type = ["cdylib"]`（给 Android）+ `["rlib"]`（给测试）。
- `rust/.cargo/config.toml` 固定 `linker = "$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android26-clang"`。
- `ring` 需 `CC_aarch64_linux_android` = 上面的 clang（**这是最容易踩的坑**，doctor 会自检）。
- `sqlx` 用 `runtime-tokio-rustls` + `sqlite` 特性，**不开宏**（`query!` 宏需要 DB 连接，CI 没有）。

### 2.3 Go（模块 5）

```bash
cd go && GOOS=android GOARCH=arm64 CGO_ENABLED=1 \
  CC=$NDK/.../aarch64-linux-android26-clang go build ./...
gomobile bind -target=android/arm64 -androidapi 26 -o $OUT/go/search.aar ./bind
```

- **只绑 `bind` 一层**（面积极小），业务包不直接暴露给 Kotlin/Dart。
- 依赖锁定 `go.sum`；cargo/go 都走 `--locked` / `-mod=readonly`（CI 防漂移）。

### 2.4 Zig（模块 4）

```bash
zig build -Dtarget=aarch64-linux-android -Doptimize=ReleaseFast \
  --prefix $OUT/zig
```

- 只产出 `libyieyu_mem.a`（分配器 + 内存池），**不产 executable、不碰 libc++ 边界**。
- 同时作为"备用 CC"（`zig cc`）在 `doctor --alternative` 里演示可用性，但不进主链路。

### 2.5 TypeScript（模块 6）

```bash
cd ts && npm ci --no-audit --no-fund
npx tsc -p tsconfig.json --outDir $OUT/ts/esm      # 类型检查 + ESM 产出
npx esbuild $OUT/ts/esm/clean/index.js --bundle --format=esm --outfile=$OUT/ts/cleaner.mjs
```

- `zod` 是唯一真源；`Song` 类型由 `z.infer` 导出，**不许手写第二份 interface**。
- 产出单个 `cleaner.mjs`：既是 WasmEdge-JS guest 的入口，也是 Go 侧兜底实现的对照基准。

### 2.6 Wasm（模块 8）

```bash
cargo build --manifest-path wasm/fx_tremolo/Cargo.toml --release --target wasm32-wasip1
# 产物 → app/assets/plugins/fx_tremolo.wasm
```

### 2.7 flatc（模块 21，所有跨语言类型）

```bash
flatc --cpp --dart --rust --go --gen-object-api \
      -o $OUT/gen idl/jsb.fbs idl/frames.fbs
```

> **契约变更流程**：改 `idl/*.fbs` → 跑 `./build.sh idl` → 提交 `docs/CONTRACTS.md` 里的版本号 +1。
> 任何一端手改生成代码 = 打回。

### 2.8 Gradle（模块 22，全场唯一 Kotlin）

```bash
./gradlew :platform-service:assembleRelease -p android
```

### 2.9 Flutter（模块 1/2）

```bash
cd app && flutter pub get
flutter build apk --release --target-platform android-arm64 \
  --dart-define=YIEYU_NATIVE_DIR=$OUT/native \
  --dart-define=YIEYU_RUST_DIR=$OUT/rust
```

---

## 3. 交叉编译的三个真实坑（已经写进脚本）

| 坑 | 现象 | 脚本里的处理 |
|---|---|---|
| `ring` 的 C 部分用宿主 `cc` | `undefined reference to __aarch64_*` / 编出 x86 目标 | `orchestrate.mjs` 强制注入 `CC_aarch64_linux_android`、`AR_aarch64_linux_android` |
| `sqlx` 默认 `native-tls` 拉 OpenSSL | 交叉编译炸 | workspace 固定 `runtime-tokio-rustls`，feature 白名单 |
| FFmpeg 预编译 ABI 与 NDK STL 不一致 | 运行期 `abort: cannot locate symbol` | `FindFFmpeg.cmake` 校验 `.so` 的 `NEEDED` + 只接受 `c++_shared` 版本；否则退静态 |

---

## 4. 环境变量约定

| 变量 | 默认 | 说明 |
|---|---|---|
| `ANDROID_NDK` | `$ANDROID_SDK_ROOT/ndk/27.0.12077973` | doctor 必检 |
| `ANDROID_API` | `26` | AAudio 门槛 |
| `OUT` | `./out` | 所有产物根 |
| `YIEYU_JOBS` | `4` | 并行度 |
| `YIEYU_OFFLINE_ASSETS` | `ON` | 无 third_party 时编空实现，骨架可先跑通 |
| `YIEYU_OBFUSCATE` | `OFF` | [11] 只在 release 打标时 ON |

---

## 5. 增量与缓存

- 调度器对每步记录 `{cmd, inputs-mtime-hash, outputs}` → 命中则 `SKIP`。
- CI 缓存四块：`~/.cargo`、`~/.gradle`、`~/.pub-cache`、`$OUT/native`（ccache）。
- `./build.sh clean <step>` 只清某一步产物（不 `rm -rf out` 全清，省 20 分钟）。

---

## 6. CI（`.github/workflows/ci.yml`）

- **只有 `workflow_dispatch`**（私有仓库分钟数要省），可传 `jobs=native,go,ts` 选跑。
- 骨架期默认跑 `doctor + tsc + go build + cargo check`（不跑 NDK/APK，因为 third_party 未入库）。
- 等 P2 把 FFmpeg/JUCE 依赖固定后，再开 `assemble` job 出 APK。

---

## 7. 现在这个骨架的真实状态（不吹）

- ✅ 目录、契约、调度器、CMake/Cargo/Go 骨架文件全部在位
- ⚠️ **还没编过一次**：本机（Android 手机）无 NDK/Rust/Go/Flutter，`third_party/` 只有说明
- 下一步：桌面机或 CI 上跑 `./build.sh doctor` → 按 doctor 的缺口补工具链
  → `./build.sh zig rust idl native YIEYU_OFFLINE_ASSETS=ON` 先让 `libyieyu_core.so` 出得来

---

## 8. 极速增量构建（把 15 分钟压到分钟级）

### 8.1 五层缓存

| 层 | 手段 | 命中时省掉 |
|---|---|---|
| **产物** | `actions/cache` 缓存整个 `out/`（含 `.build-manifest.json`） | 全部编译；调度器指纹发现没变 → 直接 SKIP |
| **C/C++/ASM** | `ccache`（`CMAKE_*_COMPILER_LAUNCHER`） | 未改动 TU 的 obj 生成 |
| **Rust** | `sccache`（`RUSTC_WRAPPER`） | 未改动 crate 的 codegen |
| **Go** | `~/.cache/go-build` + `~/go/pkg/mod` | 整个 build 与依赖下载 |
| **Flutter/Gradle** | `~/.gradle/caches`、`~/.gradle/wrapper`、`~/.pub-cache` + `subosito/flutter-action` 自带缓存 | gradle 冷启动 + pub 拉包（这是 APK 步骤从 11 分钟降到 3 分钟的关键） |

`out/` 的缓存 key 是**全部源码的 hash**：
命中 → 一个编译进程都不起；只改一个模块 → 命中 `restore-keys` 拿到上次的 `out/`，
调度器按指纹只重编那一块。

### 8.2 模块独立化（真的不交叉）

`dorny/paths-filter` 先判变更，再决定跑不跑：

| 只改了 | 会执行 | 不会碰 |
|---|---|---|
| `go/**` | `./build.sh flutter`（内部只编 go → 重新打 APK） | C++、Rust、Zig、TS |
| `ts/**` | 同上，只重生 `cleaner.mjs` | C++、Rust、Go |
| `core/**` `rust/**` `zig/**` `idl/**` | `./build.sh zig idl rust native` + 重打 APK | Go、TS |
| 什么都没改 | 仅恢复 `out/` + 上传产物 | 全部 |

> 前提：`out/` 缓存命中。缓存被淘汰时会自动退回全量，不会产出坏的包。

### 8.3 增量补丁包

`out/apk/YieyuUltimate-patch-<sha>.zip`：只含 `jniLibs/arm64-v8a/*.so` + `manifest.json`（含每文件 sha256）+ README。

**必须说清楚**：Android 10+ 非 root 设备**无法**用它覆盖已装 APK 里的 `.so`
（`classloader-namespace` 只允许加载 `/data/app/.../lib`）。

它的真实用途：
- CI 内部换了 `.so` 快速重打包（不重跑 C++/Rust）
- root / adb 可写时覆盖后重启生效
- 出问题时单独取 `.so` 做符号对比或回滚

### 8.4 脚本热更（唯一真正的"秒生效"）

改搜歌规则**完全不走打包流程**：

1. 把 `xxx.lua` 丢进 `/storage/emulated/0/yieyu/scripts/`
2. App → 设置 → **重新加载音源脚本**
3. 脚本立即进入沙箱（白名单 API、5000 万指令上限）

底层链路：`Yieyu_ReloadScripts()` → 扫目录 → 逐文件黑名单校验
（与 `tools/collect-lua.mjs` 同一份规则）→ 登记 → 供 `lua_sandbox_run()` 使用。
被拒的脚本**不登记、不执行**，UI 只显示"已加载 N 个脚本"。

> 依赖：真实执行需要 `third_party/lua`（LuaJIT 2.1）入库。现在是空实现，
> 但**扫描/校验/计数已在跑**，热更链路能先验证通。

### 8.5 实测预期（不是拍脑袋，是缓存原理决定的）

| 场景 | 预期 | 说明 |
|---|---|---|
| 无改动重跑 | **1.5 ~ 2.5 分钟** | 只剩 checkout + 恢复缓存 + 上传 |
| 只改 `go/**` | 3 ~ 5 分钟 | Go AAR + APK 重打（gradle 热） |
| 只改 `core/**` | 6 ~ 9 分钟 | Rust 免编 + C++ 走 ccache + APK 重打 |
| 冷启动（无任何缓存） | 15 ~ 18 分钟 | 只发生一次，之后都靠缓存 |
| 换了依赖版本 | 10 ~ 15 分钟 | ccache/sccache 全部失效 |

> "1 分钟"只有"什么都没改、点一下重跑"这一个场景能到；
> 真实改代码的场景下限是 3 分钟（Flutter 重打 APK 这一段省不掉）。
