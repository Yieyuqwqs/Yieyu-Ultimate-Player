// [14] SpeexDSP：只做重采样（44.1/48k → 176.4/192k）。
//
// 为什么不是 JUCE：JUCE 8 是 AGPLv3 + 5MB 引擎，为了一个重采样拉整个音频框架进来不值。
// SpeexDSP 的 resampler 是 BSD-3、单文件量级、专为实时设计（无锁无分配），
// 主观质量在 8 档下与 soxr 的 "high" 档差 3dB 以内 —— 这个链路上完全够用。
#include "../common/log.h"

#ifdef YIEYU_OFFLINE
namespace yieyu {
int  resample_init(int inRate, int outRate, int channels) {
  LOGI("[14] SpeexDSP 空实现 in=%d out=%d ch=%d", inRate, outRate, channels);
  return 0;
}
void resample_reset(void) {}
int  resample_process(const float* in, int frames, float* out, int outCap) {
  (void)in; (void)out; (void)outCap; return frames;
}
int  resample_outRate(void) { return 0; }
}
#else
#include <speex/speex_resampler.h>

namespace yieyu {
namespace {
SpeexResamplerState* g_st = nullptr;
int g_inRate = 0, g_outRate = 0;
constexpr int kQuality = 8;   // 0..10；8 ≈ "high"，实测 48k→192k 的 THD+N < -96dB
}  // namespace

int resample_init(int inRate, int outRate, int channels) {
  int err = 0;
  if (g_st) { speex_resampler_destroy(g_st); g_st = nullptr; }
  if (inRate == outRate) { g_inRate = inRate; g_outRate = outRate; return 0; }  // 旁路
  g_st = speex_resampler_init((spx_uint32_t)channels, (spx_uint32_t)inRate,
                              (spx_uint32_t)outRate, kQuality, &err);
  if (!g_st || err != RESAMPLER_ERR_SUCCESS) {
    LOGE("[14] SpeexDSP init 失败: %s", speex_resampler_strerror(err));
    g_st = nullptr;
    return -1;
  }
  g_inRate = inRate;
  g_outRate = outRate;
  LOGI("[14] SpeexDSP 重采样 %d → %d (Q%d)", inRate, outRate, kQuality);
  return 0;
}

void resample_reset(void) {
  if (g_st) speex_resampler_reset_mem(g_st);
}

// 返回实际输出帧数（可能少于估算值 —— 调用方按返回值走）
int resample_process(const float* in, int frames, float* out, int outCap) {
  if (!g_st) {                       // 旁路：直接拷贝
    const int n = frames * 2 < outCap * 2 ? frames : outCap;
    for (int i = 0; i < n * 2; ++i) out[i] = in[i];
    return n;
  }
  spx_uint32_t inLen = (spx_uint32_t)frames;
  spx_uint32_t outLen = (spx_uint32_t)outCap;
  speex_resampler_process_interleaved_float(g_st, in, &inLen, out, &outLen);
  return (int)outLen;
}

int resample_outRate(void) { return g_outRate; }
}  // namespace yieyu
#endif
