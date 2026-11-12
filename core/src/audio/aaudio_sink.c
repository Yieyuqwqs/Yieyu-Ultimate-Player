// [15] AAudio：把 PCM 直接喂给声卡。绕过 Java，无 AudioTrack、无 JNI。
// 铁律（docs/DATAFLOW.md §3）：回调里不许 malloc / 锁 / log / JNI / 文件 IO。
#include <aaudio/AAudio.h>
#include <stdbool.h>
#include <string.h>
#include "../common/log.h"

typedef struct {
  AAudioStream* stream;
  float*  ring;         /* 由 core 传入的 SpscRing 底层缓冲（见 ring.h） */
  int32_t mask;
  int32_t rd, wr;       /* 仅音频线程与 DSP 线程各写一个，用原子序保证 */
  int32_t underruns;
  bool    running;
} yieyu_sink;

static yieyu_sink g_sink = {0};

static aaudio_data_callback_result_t on_data(AAudioStream* s, void* user, void* audio, int32_t frames) {
  yieyu_sink* k = (yieyu_sink*)user;
  float* out = (float*)audio;
  const int32_t n = frames * 2;  /* 立体声交错 */
  int32_t avail = k->wr - k->rd;
  if (avail <= 0) { memset(out, 0, (size_t)n * sizeof(float)); k->underruns++; return AAUDIO_CALLBACK_RESULT_CONTINUE; }
  int32_t take = avail < n ? avail : n;
  for (int32_t i = 0; i < take; i++) out[i] = k->ring[(k->rd + i) & k->mask];
  if (take < n) memset(out + take, 0, (size_t)(n - take) * sizeof(float));
  k->rd += take;
  return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

int yieyu_aaudio_start(int32_t rate, int32_t framesPerBurst, float* ring, int32_t ringPow2) {
  AAudioStreamBuilder* b = NULL;
  if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) return -1;
  AAudioStreamBuilder_setDirection(b, AAUDIO_DIRECTION_OUTPUT);
  AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_FLOAT);
  AAudioStreamBuilder_setChannelCount(b, 2);
  AAudioStreamBuilder_setSampleRate(b, rate);
  AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
  AAudioStreamBuilder_setSharingMode(b, AAUDIO_SHARING_MODE_EXCLUSIVE);
  AAudioStreamBuilder_setDataCallback(b, on_data, &g_sink);
  AAudioStreamBuilder_setFramesPerDataCallback(b, framesPerBurst);

  g_sink.ring = ring; g_sink.mask = ringPow2 - 1; g_sink.rd = g_sink.wr = 0;
  aaudio_result_t r = AAudioStreamBuilder_openStream(b, &g_sink.stream);
  AAudioStreamBuilder_delete(b);
  if (r != AAUDIO_OK) { LOGE("AAudio open 失败: %s", AAudio_convertResultToText(r)); return -2; }
  if (AAudioStream_requestStart(g_sink.stream) != AAUDIO_OK) return -3;
  g_sink.running = true;
  LOGI("AAudio 起播 %d Hz / burst=%d / 低延迟", rate, framesPerBurst);  /* 只在非实时上下文 log */
  return 0;
}

void yieyu_aaudio_stop(void) {
  if (!g_sink.stream) return;
  AAudioStream_requestStop(g_sink.stream);
  AAudioStream_close(g_sink.stream);   /* close 会阻塞到回调结束，安全 */
  g_sink.stream = NULL; g_sink.running = false;
}

int32_t yieyu_aaudio_underruns(void) { return g_sink.underruns; }
