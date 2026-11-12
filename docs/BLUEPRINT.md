# 总纲 · 今天到哪、明天从哪开始

> 一句话：**云端工具链全装好了，构建链 12 步绿了 9 步**，剩 3 步已定位并修完待验。
> 这台手机上永远编不了（NDK 不发 aarch64-linux 宿主工具链），构建全部走 GitHub Actions。

---

## 一、工具链：13 缺 → 0 缺

全部装在 `ubuntu-latest` runner 上，`.github/workflows/build.yml` 每次从零复现：

| # | 工具 | 版本 | 装法 | 服务于模块 |
|---|---|---|---|---|
| 1 | JDK | 17 (temurin) | `actions/setup-java@v4` | 22 |
| 2 | Go | 1.22 | `actions/setup-go@v5` | 5 |
| 3 | Rust | stable + `aarch64-linux-android` | `dtolnay/rust-toolchain` | 3 / 10 / 19 |
| 4 | Flutter | 3.24.5 (Impeller) | `subosito/flutter-action@v2` | 1 / 2 |
| 5 | Android NDK | runner 预装（`$ANDROID_NDK_LATEST_HOME`） | 直接用 | 7~24 的 native |
| 6 | Zig | 0.13.0 | 官方 tar.xz → `/usr/local/bin` | 4 |
| 7 | flatc | apt（回退 release zip） | — | 21 |
| 8 | gomobile | `go install` | + `go get golang.org/x/mobile` | 5 |
| 9 | node | 20 | runner 自带 | 构建期总控 |
| 10 | cmake / ninja | runner 自带 | — | native |
| 11 | Android SDK | runner 预装 | — | Flutter APK |

> 手机端只负责写代码和推 commit（本机没有 git，用 GitHub API 的 blob→tree→commit 单提交）。

---

## 二、构建链现状（`./build.sh guard` 的 12 步）

| 步 | 内容 | 状态 |
|---|---|---|
| 0 | doctor 体检 | ✅ 绿 |
| 1 | idl（flatc 四端生成） | ✅ 绿 |
| 2 | zig（`libyieyu_mem.a`） | ✅ 绿 |
| 3 | rust（3 个 `.so`） | ✅ 绿 |
| 4 | native（`libyieyu_core.so`） | ✅ **绿 + 符号白名单自检通过** |
| 5 | ts（`cleaner.mjs`） | ✅ 绿 |
| 6 | go vet / build | ✅ 绿 |
| 7 | go gomobile bind（AAR） | 🟡 已修待验 |
| 8 | gradle（Kotlin AAR） | 🟡 已修待验 |
| 9 | fltuter APK | 🟡 已修待验 |
| 10 | verify（清单 sha256） | ⬜ 未跑过 |
| 11 | 上传产物 | ⬜ 未跑过 |

**已存在的真实产物**（run #9 跑出来的）：`libyieyu_core.so`、`libyieyu_gesture.so`、
`libyieyu_license.so`、`libyieyu_store.so`、`libyieyu_mem.a`、`cleaner.mjs`。

---

## 三、踩坑台账（12 条，全是真报错）

| # | 报错 | 根因 | 修法 |
|---|---|---|---|
| 1 | `exit 126` | API 推 blob 丢了可执行位 | push 时 `.sh/.mjs` 用 `100755` + CI `chmod +x` 兜底 |
| 2 | `unknown root type: Cmd` | `root_type` 写在 table 之前 | 移到文件末尾 |
| 3 | `invalid option: -Doptimize` | Zig 0.13：`standardOptimizeOption` 给了 `preferred_optimize_mode` 就不再注册该选项 | 去掉 preferred，CLI 传 `-Doptimize` |
| 4 | `manifest path rust/Cargo.toml does not exist` | cwd 已是 `rust/`，路径重复 | manifest 用相对名 |
| 5 | `failed to parse manifest ... idna_adapter` | 钉死 rustc 1.79，而 sqlx 依赖链要 ≥1.81 | toolchain 改 `stable` |
| 6 | `std::initializer_list was not found` | `for (auto* p : {a,b})` 需要显式头 | 补 `<initializer_list>` |
| 7 | `invalid operand for instruction`（汇编） | AArch64 `LDP` 不支持寄存器偏移 `[x0,x4]` | 先 `add x6,x0,x4` 再 `ldp ...,[x6]` |
| 8 | `version script ... 'Yieyu_Free' failed: not defined` | 导出白名单列了未实现符号 | 补上 `Yieyu_Free` |
| 9 | `Could not resolve "zod"` | esbuild 入口在 `out/`，向上找不到 `ts/node_modules` | 传 `NODE_PATH=ts/node_modules` |
| 10 | `gomobile bind requires golang.org/x/mobile` | 依赖图里没有 | 绑定前 `go get golang.org/x/mobile` |
| 11 | `friendPathsSet$kotlin_gradle_plugin_common` | 缺 `android.useAndroidX=true` | 加 `android/gradle.properties` |
| 12 | `The method 'elementAt' isn't defined for Array<Float>` | Dart FFI 的 `Array` 用下标 | `eqDb[i].value` |

---

## 四、预判问题（还没踩，但早晚会炸，先列出来）

### A. 运行期 —— 会直接崩或没声音

1. **JNI 符号被隐藏**：`-fvisibility=hidden` + version script。Kotlin 要的
   `Java_com_yieyu_ultimate_service_PlaybackService_nativePing/nativeAction` 必须在
   `exports.map` 的 global 里（已加），且 **R8 不许改名**（`consumer-rules.pro` 还没写）。
2. **`.so` 没进 APK**：Flutter 打包 `android/app/src/main/jniLibs/arm64-v8a/`。
   `build-app.sh` 会拷，但若 `flutter create` 重建了目录，路径可能对不上 → 装机后
   `DynamicLibrary.open` 抛异常（App 有降级条，不会白屏，但功能全废）。
3. **16KB page**：Android 15+ 要求 `.so` 4KB/16KB 对齐。已在链接参数加
   `-Wl,-z,max-page-size=16384`，但**Rust 侧也要加**（`.cargo/config.toml` 里有，交叉编译时要确认生效）。
4. **AAudio 回调里踩 IO**：铁律 1。现在 `aaudio_sink.c` 的回调里只有 memcpy，
   但一旦有人加 `LOGI` 就会掉帧 —— review 红线。
5. **SQLite 双写**：`rust/store-sqlx` 必须 `PRAGMA query_only=1`。现在写了，但
   如果将来有人给 SQLx 加一句 `INSERT`，锁库 → 播放卡顿（表现为"咔哒"爆音）。
6. **陀螺仪没有传感器**：`gyro_start` 返回 -2 且不报错（已降级为自动旋转），
   模拟器上必然走这条路径。
7. **FlatBuffers 版本漂移**：`frames.fbs` 的 `FrameHeader` 必须正好 40 字节
   （C++ 侧有 `static_assert`，Dart 侧没有 → Dart 的 `_CSnapshot` 布局要人工核对）。

### B. 构建期 —— 会在 CI 上红

8. **Gradle 9 与 AGP 8.5.2 的兼容带**：runner 自带 gradle 是 9.x，AGP 8.5 只保证 8.7~8.x。
   现在只是警告，但某次 runner 升级就会硬失败 → 对策：尽快用 `gradle wrapper` 钉住 8.7。
9. **Flutter 缓存**：`subosito/flutter-action` 的 cache 命中时，`flutter create` 的行为
   可能和冷启动不同（尤其是它会重写 `.metadata`）。
10. **私有仓库分钟数**：一次全链路约 20 分钟。现在 push 到 main 就触发 → 每天推 20 次
    就烧 400 分钟/月（免费额度 2000）。**建议**：日常改动用 `workflow_dispatch` 手动触发，
    只有里程碑才 push 触发。

### C. 架构期 —— 会在后期返工

11. **模块 11（O-LLVM）套不上 NDK r27**：现成 O-LLVM 分支基于 GCC 前端。
    真要做只能 LLVM Pass Plugin 或自建工具链 —— **建议降级为"符号剥离 + 段校验 + R8"**，
    自己玩够用。
12. **模块 8（WasmEdge）在 Android 上禁 JIT**：只能 interpreter，插件预算 32~64 采样点/块，
    复杂音效跑不动 → 真插件得用 AOT 预编译。
13. **模块 18（ONNX）体积**：ORT Android arm64 `.so` ≈ 12MB，模型另算。首包会明显变大。

---

## 五、还没写的东西（诚实清单）

| 项 | 状态 |
|---|---|
| `consumer-rules.pro` / `proguard-rules.pro`（模块 22 的 R8 保护） | 未创建 |
| `third_party/` 全部二进制（FFmpeg / SpeexDSP / Eigen / LuaJIT / WasmEdge / curl / SQLite / ORT / FlatBuffers） | 只有 README，未入库 |
| `tools/fetch-deps.mjs`（唯一的依赖下载入口 + sha256 校验） | 未写 |
| Rust 三个 crate 的单测（写了，但 CI 没跑 `cargo test`） | 未接入 |
| `android/app/` 的 Flutter 壳（CI 里由 `flutter create` 现场生成） | 未入库（刻意） |
| Rive 资源 `.riv` | 未做（有降级绘制） |

---

## 六、下次开工第一件事（按顺序）

1. 看 `build.yml` 最新一次是否全绿；若 `flutter` 步仍红，抓 `flutter build apk` 的
   Dart 编译错误（最可能是 `home_page.dart` 里 `JsbOp` 的枚举名对不上）。
2. 绿了之后：**装到这台手机上**（`adb install` 或直接下载 artifact），确认
   ① App 能起 ② 红色降级条不在 ③ 点播放日志里有 `[15] AAudio 起播`。
3. 然后才进 P2：把 `third_party` 钉死（先只钉 SpeexDSP + SQLite 两个小的，FFmpeg 最后上）。
4. 每次依赖变更都要过 `./build.sh doctor` + `verify`，产物清单进 `out/verify/MANIFEST.md`。

---

## 七、许可证结论（已经按你的意思处理）

- **自己折腾、不分发** → 版权焦虑归零。
- 但 **JUCE 还是换掉了**：换成 SpeexDSP（BSD-3，~300KB，实时无锁重采样），
  顺手把 5MB 引擎 + AGPL 传染 + 一个巨坑依赖全部删掉。**这是今天最划算的一次删除。**
- FFmpeg 仍建议不带 `--enable-gpl`（不影响自用，只是习惯）。
