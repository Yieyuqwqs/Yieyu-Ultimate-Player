// [24] 自研 JSB 引擎：UI ↔ 底层唯一合法入口。
// Dart 侧走 dart:ffi（不是 JNI，模块 22 的 Kotlin 才用 JNI），所以这层叫 JSB 而不是 JNI 桥。
// 控制面：FlatBuffers（idl/jsb.fbs）。数据面：SpscRing + 共享内存头（idl/frames.fbs）。
#pragma once
#include <cstddef>
#include <cstdint>

#define YIEYU_JSB_ABI 1

// ★ 必须显式标 default：本库用 -fvisibility=hidden 编译，hidden 符号 linker
//   无论如何都不会进 .dynsym，version script 也提不起来 —— 这导致过
//   "dlopen 成功但 lookup Yieyu_Init 失败"（踩过，别删）。
#if defined(__GNUC__) || defined(__clang__)
#  define YIEYU_API __attribute__((visibility("default")))
#else
#  define YIEYU_API
#endif  // 与 idl/jsb.fbs 的 version 字段必须一致，改契约就 +1

extern "C" {

// ① 生命周期。filesDir 由 Kotlin/Flutter 侧传入（不许自己猜路径）
YIEYU_API int  Yieyu_Init(const char* filesDir, const char* abi /* "arm64-v8a" */, uint32_t flags);
YIEYU_API void Yieyu_Shutdown(void);

// ② 控制面下行：Flutter → core。fbs 已按 idl/jsb.fbs 序列化，core 不反序列化未知字段
YIEYU_API int Yieyu_Post(const uint8_t* fbs, size_t len);

// ③ 控制面上行：core → Flutter。返回写入字节数；0 表示暂无事件。
//    调用方提供缓冲区（Dart 持有），不跨边界分配 → 零 GC 压力
YIEYU_API size_t Yieyu_Poll(uint8_t* out, size_t cap);

// ④ 数据面：可视化/进度用的只读快照（PCM 环 + 峰值），由 Flutter 每帧拉一次
struct Yieyu_Snapshot {
  int32_t  playing;       // 0/1
  int32_t  sampleRate;    // 44100 / 48000 / 192000
  int64_t  positionMs;
  int64_t  durationMs;
  float    peakL, peakR;
  int32_t  eqPoints;      // 模块 17 提供的 31 点（喂模块 2 的 Rive 曲线）
  float    eqDb[31];
};
typedef struct Yieyu_Snapshot Yieyu_Snapshot;
YIEYU_API int Yieyu_Snapshot_Get(Yieyu_Snapshot* out);

// ⑤ 给模块 22（Kotlin 前台服务）的最小面：只读状态，不含业务判定
YIEYU_API int  Yieyu_Service_Ping(void);
YIEYU_API void Yieyu_Service_OnNotificationAction(int actionId);  // 0=play 1=pause 2=next 3=prev

// ⑥ 诊断
YIEYU_API void        Yieyu_Free(void* p);   // 释放 core 分配的内存（Dart 侧用完必须调，否则泄漏）
YIEYU_API int         Yieyu_Version(void);
YIEYU_API const char* Yieyu_LastError(void);
YIEYU_API void        Yieyu_Stats(char* out, size_t cap);   // 各模块健康度一行一模块

// ── 黑匣子（模块 0：日志基础设施）────────────────────────────────
// UI 永远不许展示里面的技术内容；这里只给"导出"和"拿到目录"两个动作。
YIEYU_API int  Yieyu_ExportLogs(char* outDir, size_t cap);   // 0=成功，outDir 里是最终目录
YIEYU_API int  Yieyu_LogDir(char* outDir, size_t cap);
YIEYU_API int  Yieyu_LogIsPublic(void);                      // 1=真的写进了 /storage/emulated/0/yieyu
YIEYU_API void Yieyu_SetTrace(const char* traceId);

// ⑦ 播放控制：能放歌的最小闭环（URI → [13] 解码 → [15] 出声）。
//    语义：**异步受理** —— 返回 0 只代表请求交给了解码线程，不代表已经出声。
//    注意：打开流可能阻塞在网络 IO 上，所以这条永远不会在调用线程里解码。
YIEYU_API int  Yieyu_PlayUrl(const char* uri);   // 0=已受理；<0=参数错
YIEYU_API void Yieyu_Pause(void);
YIEYU_API void Yieyu_Resume(void);
YIEYU_API void Yieyu_Stop(void);
YIEYU_API int  Yieyu_SeekMs(int64_t ms);         // 0=已受理
YIEYU_API void Yieyu_SetVolume(float v);         // 0..1

// ⑧ [17] DSP 音效链。默认全关（旁路）——打开后才会有音色变化。
YIEYU_API void Yieyu_SetDsp(int on);                        // 1=EQ+胆机/黑胶+8D 全开
YIEYU_API void Yieyu_SetEqGain(int band, float db);         // band 0..30，±24 dB
YIEYU_API void Yieyu_SetTone(int tube, int vinyl, int lpf, float drive);

// ⑬ [14] 输出采样率：0=自动（PCM 跟源、DSD→192k），或 48000/96000/192000 强制升频。
//    打开后整条链路（解码 → [14] → [17] → [15]）都按目标率跑，声卡也按目标率开流。
YIEYU_API void Yieyu_SetRate(int hz);

// ⑭ [9] 自研取流自检：对 http:// 发一次请求，返回 HTTP 状态码，信息写进 out（诊断面板显示）。
YIEYU_API int Yieyu_HttpProbe(const char* url, char* out, int cap);

// ⑮ [18] 离线人声消除（生成伴奏）：异步受理，状态用 Yieyu_KaraokeStatus 查。
//    MakeKaraoke: 0=已受理 <0=参数错/已在跑；KaraokeStatus: -2 空闲 -1 运行中 0 完成 <0 失败码
YIEYU_API int Yieyu_MakeKaraoke(const char* srcWav, const char* dstWav);

// ⑯ [8] wasm 插件：导入（字节）与开关。name 必须以 .wasm 结尾。
YIEYU_API int Yieyu_ImportPlugin(const char* name, const uint8_t* data, int len);
YIEYU_API void Yieyu_SetPlugin(int on);
YIEYU_API int Yieyu_KaraokeStatus(char* out, int cap);

// ⑨ [5] 搜歌（真实网络，走 Go 编的 libyieyu_fetch.so）。
//    0=已受理（异步，网络在独立线程）；结果从 Yieyu_Poll 取：
//      事件帧 = [0] 类型 'S'=结果JSON数组 / 'E'=错误文案
//               [1..4] 小端 uint32 载荷长度
//               [5..]  UTF-8 载荷
//    搜索结果元素：{"id":"95328","mid":"002K6...","name":"晴天","artist":"周杰伦",
//                  "album":"叶惠美","source":"wy"}
YIEYU_API int Yieyu_Search(const char* keyword);

// ⑩ [5] 点歌：把 (source, musicId, mid) 交给播放器，直链解析在解码线程做（控制面不等网络）。
//    0=已受理；<0=参数错。失败原因进 Yieyu_LastError()。
YIEYU_API int Yieyu_PlaySong(const char* source, const char* musicId, const char* mid);

// ── 模块 7 热更：脚本导入（不打包、不重启，秒生效）─────────────────
// 扫描 /storage/emulated/0/yieyu/scripts/*.lua，黑名单校验后加载。
// loaded/rejected 可传 NULL。返回 0=完成。
YIEYU_API int Yieyu_ReloadScripts(int* loaded, int* rejected);

// 调脚本函数（音源脚本的 get_music_url 就走这里）。
// a1..a3 可为 NULL（表示该参数不存在）。结果写进 out。0=成功，<0=见 lua_sandbox_call。
YIEYU_API int Yieyu_ScriptCall(const char* script, const char* fn,
                               const char* a1, const char* a2, const char* a3,
                               char* out, int cap);

// 界面导入（SAF 选完文件后直接喂字节，不需要用户去翻存储路径）：
//   name 以 .lua 结尾；data/len 是文件内容；写入私有目录后立刻注册进沙箱。
//   返回 0=成功，-1=参数错，-2=触发黑名单，-3=写盘失败，-4=超限(>512KB)
YIEYU_API int Yieyu_ImportScript(const char* name, const uint8_t* data, int len);


// ── ⑪ [11] 防篡改自校验（期望摘要由构建期生成，随 APK 资产下发）─────────
//   IntegrityCheck: 1=一致 0=磁盘上的 so 被改过 -1=取不到自身
//   SelfDigest: 把自己 .so 的 sha256 写成 64 位小写 hex
YIEYU_API int Yieyu_IntegrityCheck(const char* expectHex);
YIEYU_API int Yieyu_SelfDigest(char* out, size_t cap);

// ── ⑫ 存储两侧计数（[20] 写者 / [19] 只读库），诊断面板用 ─────────────
YIEYU_API int Yieyu_KvCount(void);
YIEYU_API int Yieyu_StoreCount(void);

}
