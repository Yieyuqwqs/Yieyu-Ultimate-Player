# HANDOFF · 接手必读

## 0. 一句话

`Yieyuqwqs/Yieyu-Ultimate-Player`：**24 个模块全部有真实调用点，零第三方二进制**。
所有"空实现"分支已删除；缺的库用自研等价实现替掉（清单见 `third_party/README.md`）。

## 1. 运行环境（本机 vs 云端）

| 项 | 值 |
|---|---|
| 仓库 / 分支 | `Yieyuqwqs/Yieyu-Ultimate-Player` / `main`（public） |
| 本机工作副本 | `~/work/up/repo`（Android 手机上，**没有 git/NDK/Rust/Go/Flutter/Python**） |
| 本机有什么 | node（构建脚本 + 本地镜像测试）、Kotlin/Python 都没有 |
| 唯一编译环境 | GitHub Actions（`.github/workflows/build.yml`，一次 ~15 分钟） |
| 推送方式 | `node ~/work/up/push.js <commit-message-file>`（全量树提交，支持删除文件） |
| 看构建 | `node ~/work/up/watch.js`；拉日志 `node ~/work/up/getlog.js <job_id> out.log` |
| 包名 / 最低版本 | `com.yieyu.yieyu_ultimate` / minSdk 26（AAudio 门槛） |

> **本机永远编不了**：NDK 不发布 aarch64-linux 宿主工具链，Flutter 不发 arm64 Linux SDK。
> 所以改完必须靠"本地镜像测试 + 一次云端构建"验证。

## 2. 24 个模块各自接在哪（这是本轮工作的全部内容）

| # | 接线点（真实调用链） |
|---|---|
| 1 | `app/lib/shell`（首页/设置）+ `app/lib/diag/modules_page.dart`（24 模块自检） |
| 2 | `app/lib/animation/vector_stage.dart`：弹簧状态机，输入 `Yieyu_Snapshot.eqDb` |
| 3 | `home_page` 的 Listener → `GestureBridge` → `libyieyu_gesture.so`（滑动切歌/长按换 EQ） |
| 4 | `Yieyu_Post` 命令帧先过 `yieyu_mem_alloc`（Zig 池） |
| 5 | 首页搜索 → `Yieyu_Search` → Go `yieyu_music_search`；点歌 → `yieyu://song` → `yieyu_music_url` |
| 6 | 构建期 `tools/gen-schema.mjs`：Zod schema → `app/lib/model/song.dart`（UI 直接用） |
| 7 | 脚本导入/随包 `wy.lua` → `lua_sandbox` → `get_music_url` → 播放 |
| 8 | `assets/plugins/*.wasm` → 自研解释器 → 解码线程 DSP 链（设置里可关） |
| 9 | `http://` 直链 → `yieyu_http_download` → 私有缓存 → 解码器；自检页有 `Yieyu_HttpProbe` |
| 10 | `Engine::Boot` dlopen `libyieyu_license.so` → `yieyu_license_has_pubkey` |
| 11 | `tools/obfuscate.mjs` 链接后重命名+strip+sha256 → `assets/integrity/` → 启动 `Yieyu_IntegrityCheck` |
| 12 | 上面的摘要比对走 `asm/neon_ct_compare.S`（常数时间） |
| 13 | `yieyu_pdec_open/read/seek`（mediandk；本地文件走 fd，WAV 有内置直通） |
| 14 | `Player::OpenAndPlay` 按目标率 `resample_init/process`；设置页可切 48k/96k/192k |
| 15 | `yieyu_aaudio_start` 拉数据回调 + 四级降级 |
| 16 | `dsd_probe` 命中 .dsf/.dff → `dsd_open/read`（FIR÷8）→ 模块14 |
| 17 | 解码线程：`eq31/tube_vinyl/hrtf8d`；8D 姿态来自模块 23 |
| 18 | 开发者面板「[18] 生成伴奏」→ `Yieyu_MakeKaraoke` → `yieyu_vocals_render`（核系数由 Python 生成） |
| 19 | `Engine::Boot` dlopen `libyieyu_store.so` → 只读同一 KV 文件 → 计数进 Stats |
| 20 | `kv_open/put/get`：音量/音效开关/上次曲目/播放进度（每 2 秒一次） |
| 21 | 控制面/数据面统一 40 字节帧头；`tools/verify-idl.mjs` 拿 fbs 反查偏移与枚举 |
| 22 | `patch_gradle.mjs` 打 AAR + MainActivity 拉起 → 通知栏 → JNI 2 个符号 |
| 23 | `gyro_start(200Hz)` → 解码线程每块读姿态 → `hrtf8d_set_pose` |
| 24 | `core/src/jsb`：全部导出经 `exports.map` 白名单（`llvm-nm -D` 校验） |

## 3. 本地镜像测试（改算法前先跑这个，比一次 15 分钟的构建便宜得多）

```bash
cd ~/work/up/mirror
node sha_crc.js          # sha256 / crc32 对 node 参考实现
node resampler100.js     # 模块14：100 轮随机比例/分块/声道/复位
node kv_model.js         # 模块20：100 轮 put/del/压实 + 随机断电截断
node dsd100.js           # 模块16：DSF/DFF 容器 + 位展开 + 解码（100 轮）
node dsd_two_stage.js    # 模块16：两级串联（FIR÷8 + 重采样）100 轮
node http_model2.js      # 模块9：裸 TCP 抓包 → 解析器（100 轮，含 chunked/302/404）
node vocals_model.js     # 模块18：FFT 对 DFT + 中置抑制效果
```
镜像全部通过再改 C++，这是本项目最省时间的纪律（历史上同一问题连发三版就是这么来的）。

## 4. 红线（谁改谁负责）

1. 一个模块只干表里那件事；2. 音频线程零 JNI/零 Dart/零分配/零日志；
3. 存储单写者（20 写、19 只读）；4. 密钥只在模块 10；
5. Kotlin ≤ 400 行；6. **UI 绝对静默**（任何技术报错文本出现在界面 = 事故）；
7. 成功流程不记日志，只有 ERROR/FATAL 落黑匣子。

## 5. 常见故障对照

| 现象 | 先看哪里 |
|---|---|
| 首页「出了点小状况」 | 设置页连点标题 5 次 → 开发者面板 → 启动日志（dlopen 原因） |
| 导入脚本失败 | 面板「脚本导入（诊断）」/ `import_diag.txt` / `Yieyu_LastError()` |
| 没声音 | 面板「本地测试音」→ 再试直链；状态行 `st=` / `underrun=` |
| DSD 不出声 | 面板看 `dsd=1`？`Yieyu_LastError` 里是 [16] 的哪个 rc |
| 构建红 | 先看 `out/.build-manifest.json` 判断哪一步，再拉 job 日志 |

## 6. 已知边界（不是 bug）

- 只出 arm64；`https` 不在模块 9 做（NDK 无 TLS，走系统解码器或 Go）；
- 模块 18 是"离线整曲处理"，实时人声消除需要 NN 模型，不在本轮范围；
- 模块 8 的解释器只覆盖自研插件生成器产出的指令子集（够跑音效插件，不是通用 wasm 运行时）；
- 模块 11 是"符号收敛 + 完整性自校验"，不是 OLLVM 的控制流混淆。
