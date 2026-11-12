// [24] 自研 JSB 引擎：UI ↔ 底层唯一合法入口。
// Dart 侧走 dart:ffi（不是 JNI，模块 22 的 Kotlin 才用 JNI），所以这层叫 JSB 而不是 JNI 桥。
// 控制面：FlatBuffers（idl/jsb.fbs）。数据面：SpscRing + 共享内存头（idl/frames.fbs）。
#pragma once
#include <cstddef>
#include <cstdint>

#define YIEYU_JSB_ABI 1  // 与 idl/jsb.fbs 的 version 字段必须一致，改契约就 +1

extern "C" {

// ① 生命周期。filesDir 由 Kotlin/Flutter 侧传入（不许自己猜路径）
int  Yieyu_Init(const char* filesDir, const char* abi /* "arm64-v8a" */, uint32_t flags);
void Yieyu_Shutdown(void);

// ② 控制面下行：Flutter → core。fbs 已按 idl/jsb.fbs 序列化，core 不反序列化未知字段
int Yieyu_Post(const uint8_t* fbs, size_t len);

// ③ 控制面上行：core → Flutter。返回写入字节数；0 表示暂无事件。
//    调用方提供缓冲区（Dart 持有），不跨边界分配 → 零 GC 压力
size_t Yieyu_Poll(uint8_t* out, size_t cap);

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
int Yieyu_Snapshot_Get(Yieyu_Snapshot* out);

// ⑤ 给模块 22（Kotlin 前台服务）的最小面：只读状态，不含业务判定
int  Yieyu_Service_Ping(void);
void Yieyu_Service_OnNotificationAction(int actionId);  // 0=play 1=pause 2=next 3=prev

// ⑥ 诊断
void        Yieyu_Free(void* p);   // 释放 core 分配的内存（Dart 侧用完必须调，否则泄漏）
int         Yieyu_Version(void);
const char* Yieyu_LastError(void);
void        Yieyu_Stats(char* out, size_t cap);   // 各模块健康度一行一模块

}
