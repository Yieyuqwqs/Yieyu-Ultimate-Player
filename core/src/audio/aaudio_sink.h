// [15] AAudio 出声的音箱接口。
// 只定义"谁往里拉数据"，不定义数据哪来 —— 数据面由 player.cpp 通过 Pull 回调提供。
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 拉数据的回调：填 frames 个立体声帧（交错 float），返回真正填了几帧。
// ★ 在音频线程里被调用：不许 malloc / 加锁 / 打日志 / 碰 JNI / 做文件 IO。
typedef int (*yieyu_pull_fn)(void* ctx, float* out, int frames);

// 打开并起播。rate=源采样率（不做重采样，直接按源采样率开流最省事也最准）。
// 返回 0=成功；<0=失败（已按 独占→共享 逐级降级）。
int  yieyu_aaudio_start(int32_t rate, int32_t burst, yieyu_pull_fn fn, void* ctx);
void yieyu_aaudio_stop(void);
int32_t yieyu_aaudio_underruns(void);   // 欠载次数（诊断用，非实时线程调用）
int  yieyu_aaudio_active(void);         // 1=流在跑

#ifdef __cplusplus
}
#endif
