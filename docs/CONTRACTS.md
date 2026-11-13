# 跨语言契约 · CONTRACTS

> 这份文件是**四端（Dart / C++ / Rust / Go）之间的合同**。任何一端想改字段，先改这里，再改 `idl/`，
> 最后重新生成代码。手改生成物 = review 直接打回。

---

## 0. 版本规则

| 常量 | 位置 | 规则 |
|---|---|---|
| `YIEYU_JSB_ABI` | `core/src/jsb/jsb.h` | 每次**破坏性**契约变更 +1（加字段不算破坏，改语义算） |
| `abi` 字段 | `idl/jsb.fbs` 的 `Cmd`/`Event` | 必须等于上面的常量 |
| `file_identifier` | `"YJSC"` / `"YFRM"` | 钉死，永不改；改了就是新协议，必须双跑一个版本周期 |

启动时三端自检：`Yieyu_Version()`（C++）、`JsbClient.abiVersion`（Dart）、
`search.aar` 的 `bind.Version()`（Go）。**不一致直接报错，不许"试着跑跑看"。**

---

## 1. C ABI 导出面（`libyieyu_core.so` 只有这 8 个符号）

由 `core/cmake/exports.map` 白名单控制，其余符号全部 hidden（配合模块 11 混淆）。

| 符号 | 签名 | 线程 | 说明 |
|---|---|---|---|
| `Yieyu_Init` | `int(const char* filesDir, const char* abi, uint32_t flags)` | UI 线程 | 幂等；返回 0 不代表已授权 |
| `Yieyu_Shutdown` | `void()` | UI 线程 | 幂等 |
| `Yieyu_Post` | `int(const uint8_t* fbs, size_t len)` | UI 线程 | 下行命令，内部串行化 |
| `Yieyu_Poll` | `size_t(uint8_t* out, size_t cap)` | UI 线程（16ms 泵） | 0 = 暂无事件 |
| `Yieyu_Snapshot_Get` | `int(Yieyu_Snapshot*)` | 任意 | 只读快照，无锁 |
| `Yieyu_Service_Ping` | `int()` | 任意 | Kotlin 用 |
| `Yieyu_Service_OnNotificationAction` | `void(int)` | 主线程 | Kotlin 用，只转发 |
| `Yieyu_Version` | `int()` | 任意 | ABI 版本 |
| `Yieyu_ExportLogs` | `int(char* outDir, size_t cap)` | UI 线程 | 刷盘 + 拷到 `exported/`，返回最终目录 |
| `Yieyu_LogDir` | `int(char* outDir, size_t cap)` | 任意 | 当前日志目录（可能已降级） |
| `Yieyu_LogIsPublic` | `int()` | 任意 | 1 = 真写进了 `/storage/emulated/0/yieyu` |
| `Yieyu_SetTrace` | `void(const char*)` | 任意 | 手动设 trace_id（一般由 `Yieyu_Post` 自动生成） |

> `Yieyu_Free` / `Yieyu_Stats` / `Yieyu_LastError` 已在 `exports.map` 预留，P1 补实现。

**`Yieyu_Snapshot` 定长结构（Dart 侧必须逐字段对齐）**

| 偏移 | 类型 | 字段 | 语义 |
|---|---|---|---|
| 0 | int32 | playing | 0/1 |
| 4 | int32 | sampleRate | 44100 / 48000 / 192000 |
| 8 | int64 | positionMs | 当前播放位置 |
| 16 | int64 | durationMs | 总长（未知 0） |
| 24 | float | peakL / peakR | 峰值，喂 Rive 呼吸灯 |
| 32 | int32 | eqPoints | 固定 31 |
| 36 | float[31] | eqDb | 模块 17 的 31 个 dB 点 → 模块 2 的曲线 |

---

## 2. 控制面：`Cmd` 操作表

| op | 值 | 载荷 | 谁处理 |
|---|---|---|---|
| `PLAY` | 1 | `PlayReq{song_id,url}` | core → [13]/[15] |
| `PAUSE` | 2 | — | core |
| `NEXT` / `PREV` | 3/4 | — | core（队列在 core） |
| `SEARCH` | 5 | `SearchReq{keyword,budget_ms,sources}` | [5] Go → [6] TS |
| `SEEK` | 6 | `SeekReq{position_ms}` | [13] |
| `EQ_PRESET_NEXT` | 7 | — | [17] |
| `SET_EQ_BAND` | 8 | `[EqBand{index,db}]` | [17] |
| `LICENSE` | 9 | — | [10] |
| `LOAD_PLUGIN` | 10 | — | [8] |

## 3. 控制面：`Event` 事件表

| type | 值 | 何时发 | UI 反应 |
|---|---|---|---|
| `READY` | 1 | 冷启动完成 | 建首页、播开场 Rive |
| `SEARCH_RESULT` | 2 | 搜歌返回（已洗） | 列表渲染 |
| `STATE` | 3 | 播放状态变化 | 按钮态 / 进度 |
| `LICENSE_FAIL` | 4 | 验签失败 | **置灰 + 退款提示，不崩** |
| `ERROR` | 5 | 任何模块报错 | Toast / 日志 |
| `STATS` | 6 | 心跳（30s） | 调试面板 |

---

## 4. 数据面：环头（`idl/frames.fbs` / `FrameHeader`）

40 字节定长，两端 `static_assert(sizeof == 40)`。

| 偏移 | 类型 | 字段 | 备注 |
|---|---|---|---|
| 0 | uint32 | magic | `0x5946524D` = 'YFRM' |
| 4 | uint16 | version | = ABI |
| 6 | uint16 | channels | 固定 2 |
| 8 | uint32 | sampleRate | — |
| 12 | uint32 | frames | 192 帧 = 4ms @48k |
| 16 | uint64 | seq | 单调递增，防撕裂（UI 可视化靠它判丢帧） |
| 24 | uint64 | ptsUs | 播放位置（微秒） |
| 32 | uint64 | checksum | release 置 0 |

**环的规则**：容量必须是 2 的幂；单生产者单消费者；`write/read` 返回实际数量（**永远可能短写**）。

---

## 5. 存储：单一写者

```sql
-- 写者：模块 20（C++）。WAL + synchronous=NORMAL + busy_timeout=5000
CREATE TABLE IF NOT EXISTS kv (
  k TEXT PRIMARY KEY,
  v BLOB,
  t INTEGER
);

CREATE TABLE IF NOT EXISTS track (
  id           INTEGER PRIMARY KEY,
  source       TEXT NOT NULL,      -- netease/qq/kugou/local
  title        TEXT NOT NULL,
  artist       TEXT NOT NULL DEFAULT '',
  album        TEXT NOT NULL DEFAULT '',
  duration_ms  INTEGER NOT NULL DEFAULT 0,
  quality      TEXT NOT NULL DEFAULT 'unknown',
  fingerprint  TEXT NOT NULL,      -- 去重键：source:id
  last_played  INTEGER
);
```

- 模块 19（Rust SQLx）连接必须 `PRAGMA query_only=true`，`max_connections=4`。
- 直链（`url`）**不入库**：会过期，只存内存 + 1h 租约。

---

## 6. 命名与落位规范

| 对象 | 规则 | 例 |
|---|---|---|
| 模块目录 | `core/src/<域>/`，域名=职责，不是语言 | `audio/`, `dsp/`, `script/` |
| 动态库 | `libyieyu_<模块英文名>.so` | `libyieyu_gesture.so` |
| C 符号 | `Yieyu_` 前缀（导出）/ `yieyu_` 前缀（内部 C） | `Yieyu_Post`, `yieyu_ct_equal` |
| Rust FFI | `yieyu_<域>_<动作>` | `yieyu_license_verify` |
| Go 包 | `internal/` 下，**永不** `pkg/` | `internal/fanout` |
| 产物 | 只落 `out/<语言>/` | `out/rust/aarch64-linux-android/release/` |
| 日志 tag | 统一 `yieyu`，格式 `[模块号] 消息` | `LOGI("[17] EQ31 初始化 @%d Hz", sr)` |

---

## 7. 契约变更流程（五步，缺一步就是事故）

1. 改 `docs/CONTRACTS.md`（先写清楚）
2. 改 `idl/*.fbs`
3. 若破坏性：`YIEYU_JSB_ABI` +1，并在 Dart/Rust/Go 侧同步常量
4. `./build.sh idl` 重新生成四端代码，**提交生成物**
5. 跑 `./build.sh --dry-run` + CI 的 `contract` job


---

## 8. 黑匣子日志记录格式（机器可读，别改顺序）

```
2026-11-13T08:30:12.345+08:00 | ERROR | trace=1a2b3c4d0007 | module=15 | [15] AAudio open 失败: ErrorInvalidState
    stack: frames=9 0x7a1c3f0010 0x7a1c3f22a4 ...
```

| 段 | 规则 |
|---|---|
| 时间戳 | 本地时间，毫秒，带时区偏移 |
| 级别 | 只有 `ERROR` / `FATAL` |
| trace_id | 由 `Yieyu_Post` 每条命令生成，同一链路共享 |
| module | 消息开头的 `[NN]` 自动解析（对应 24 模块序号） |
| 消息 | 必须含关键入参（路径/URL/id），不许含密钥与用户隐私 |
| stack | FATAL 保证有；ERROR 能取就取（16 帧） |

**轮转**：单文件 5MB → `.1` → `.2` → `.3`，最多 3 个历史，最旧自动删。


## 4. 控制面 / 数据面统一帧头（40 字节，模块 21）

| 偏移 | 类型 | 字段 | 说明 |
|---|---|---|---|
| 0 | char[4] | magic | 命令 `YJSC` / 事件 `YFRM`（= 两个 fbs 的 file_identifier） |
| 4 | u16 | version | 必须 = `YIEYU_JSB_ABI`（现在 1），不等就拒收 |
| 6 | u16 | channels | 命令=op（见 `idl/jsb.fbs` enum Op）/ 事件=类型（'S'=搜索结果 'E'=错误） |
| 8 | u32 | sampleRate | 数据面才有意义 |
| 12 | u32 | frames | **载荷字节数**（唯一的长度字段，不许出现第二个） |
| 16 | u64 | seq | 单调递增 |
| 24 | u64 | ptsUs | 数据面时间戳 |
| 32 | u64 | checksum | 调试期用，release 置 0 |

载荷从第 40 字节开始。CI 里 `node tools/verify-idl.mjs` 会拿 `idl/frames.fbs` 反查这张表的
偏移与枚举顺序，对不上直接红。

## 5. 本地存储记录格式（模块 20 写、19 读）

```
[magic "YKV1" u32][klen u16][vlen u32][crc32 u32][key klen][val vlen]
```

- `vlen = 0` 是删除墓碑；
- `crc32` = key 与 val 依次 update 的累计 CRC（IEEE 多项式）；
- 打开时逐条校验，遇到半条记录（断电残留）就截断到最后一条完整记录；
- 文件超过 8MB 自动压实（活记录搬到 tmp 再 rename）。
