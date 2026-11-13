// [15] AAudio：把 PCM 直接喂给声卡。绕过 Java，无 AudioTrack、无 JNI。
// 铁律（docs/DATAFLOW.md §3）：回调里不许 malloc / 锁 / log / JNI / 文件 IO。
//
// 真机经验：AAUDIO_SHARING_MODE_EXCLUSIVE 在很多机型（尤其非系统 App）会被拒，
// 所以这里按 独占+burst → 独占 → 共享+burst → 共享 四级降级，
// 任何一级成功就停；全失败才算真失败（而不是一上来就死）。
#include "aaudio_sink.h"

#include <aaudio/AAudio.h>
#include <stdatomic.h>
#include <stddef.h>
#include <string.h>

#include "../common/log.h"

static AAudioStream*   g_stream = NULL;
static yieyu_pull_fn   g_pull = NULL;
static void*           g_ctx = NULL;
static atomic_int      g_underruns;
static atomic_int      g_active;
static atomic_int      g_rate;

/* 音频线程：零分配、零日志、零锁、零 JNI —— 只有 memcpy 与一次回调 */
static aaudio_data_callback_result_t on_data(AAudioStream* s, void* user, void* audio, int32_t frames) {
  (void)s; (void)user;
  float* out = (float*)audio;
  int got = 0;
  if (g_pull) got = g_pull(g_ctx, out, frames);
  if (got < 0) got = 0;
  if (got > frames) got = frames;
  if (got < frames) {
    memset(out + (size_t)got * 2, 0, (size_t)(frames - got) * 2 * sizeof(float));
    if (got == 0) atomic_fetch_add_explicit(&g_underruns, 1, memory_order_relaxed);
  }
  return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

static aaudio_result_t open_once(int32_t sharing, int use_burst, int32_t rate, int32_t burst) {
  AAudioStreamBuilder* b = NULL;
  if (AAudio_createStreamBuilder(&b) != AAUDIO_OK || b == NULL) return AAUDIO_ERROR_INTERNAL;
  AAudioStreamBuilder_setDirection(b, AAUDIO_DIRECTION_OUTPUT);
  AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_FLOAT);
  AAudioStreamBuilder_setChannelCount(b, 2);
  AAudioStreamBuilder_setSampleRate(b, rate);
  AAudioStreamBuilder_setSharingMode(b, sharing);
  AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
  AAudioStreamBuilder_setDataCallback(b, on_data, NULL);
  if (use_burst && burst > 0) AAudioStreamBuilder_setFramesPerDataCallback(b, burst);
  g_stream = NULL;
  aaudio_result_t r = AAudioStreamBuilder_openStream(b, &g_stream);
  AAudioStreamBuilder_delete(b);
  return r;
}

int yieyu_aaudio_start(int32_t rate, int32_t burst, yieyu_pull_fn fn, void* ctx) {
  if (g_stream) yieyu_aaudio_stop();
  if (fn == NULL) return -1;
  if (rate <= 0) rate = 48000;
  if (burst <= 0) burst = 192;

  g_pull = fn;
  g_ctx = ctx;
  atomic_store_explicit(&g_underruns, 0, memory_order_relaxed);

  aaudio_result_t r = open_once(AAUDIO_SHARING_MODE_EXCLUSIVE, 1, rate, burst);
  if (r != AAUDIO_OK) {
    LOGE("[15] AAudio 独占+burst 失败: %s", AAudio_convertResultToText(r));
    r = open_once(AAUDIO_SHARING_MODE_EXCLUSIVE, 0, rate, burst);
  }
  if (r != AAUDIO_OK) {
    LOGE("[15] AAudio 独占失败: %s", AAudio_convertResultToText(r));
    r = open_once(AAUDIO_SHARING_MODE_SHARED, 1, rate, burst);
  }
  if (r != AAUDIO_OK) {
    LOGE("[15] AAudio 共享+burst 失败: %s", AAudio_convertResultToText(r));
    r = open_once(AAUDIO_SHARING_MODE_SHARED, 0, rate, burst);
  }
  if (r != AAUDIO_OK) {
    LOGE("[15] AAudio 全部打开失败: %s", AAudio_convertResultToText(r));
    g_stream = NULL;
    g_pull = NULL;
    g_ctx = NULL;
    return -2;
  }
  if (AAudioStream_requestStart(g_stream) != AAUDIO_OK) {
    LOGE("[15] AAudio start 失败");
    AAudioStream_close(g_stream);
    g_stream = NULL;
    g_pull = NULL;
    g_ctx = NULL;
    return -3;
  }
  atomic_store_explicit(&g_active, 1, memory_order_relaxed);
  atomic_store_explicit(&g_rate, rate, memory_order_relaxed);
  return 0;
}

void yieyu_aaudio_stop(void) {
  if (!g_stream) return;
  atomic_store_explicit(&g_active, 0, memory_order_relaxed);
  AAudioStream_requestStop(g_stream);
  AAudioStream_close(g_stream);   /* close 会阻塞到回调全部退出，之后清环才安全 */
  g_stream = NULL;
  g_pull = NULL;
  g_ctx = NULL;
}

int32_t yieyu_aaudio_underruns(void) { return (int32_t)atomic_load_explicit(&g_underruns, memory_order_relaxed); }
int yieyu_aaudio_active(void) { return atomic_load_explicit(&g_active, memory_order_relaxed) ? 1 : 0; }
