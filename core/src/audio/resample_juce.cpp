// [14] JUCE：只做重采样（44.1/48k → 176.4/192k）。
// 只在"高解析"档开；默认旁路，因为重采样不是音质增益而是链路选择。
#include "../common/log.h"

#ifdef YIEYU_OFFLINE
namespace yieyu {
int  resample_init(int inRate, int outRate, int channels) { (void)inRate; (void)outRate; LOGI("[14]JUCE 空实现 in=%d out=%d ch=%d", inRate, outRate, channels); return 0; }
void resample_reset() {}
int  resample_process(const float* in, int frames, float* out) { (void)in; (void)out; return frames; }
int  resample_outRate() { return 0; }
}
#else
#include <juce_dsp/juce_dsp.h>
namespace yieyu {
namespace {
  std::unique_ptr<juce::LagrangeInterpolator> g_ip[2];   // 或使用 juce::dsp::Oversampling 的多相实现
  int g_outRate = 0, g_inRate = 0;
  double g_speed = 1.0;
}
int resample_init(int inRate, int outRate, int channels) {
  g_inRate = inRate; g_outRate = outRate;
  g_speed = (double)outRate / (double)inRate;
  for (int ch = 0; ch < 2; ++ch) { g_ip[ch] = std::make_unique<juce::LagrangeInterpolator>(); g_ip[ch]->reset(); }
  (void)channels;
  LOGI("[14] JUCE 重采样 %d → %d (x%.3f)", inRate, outRate, g_speed);
  return 0;
}
int resample_process(const float* in, int frames, float* out) {
  const int outFrames = (int)(frames * g_speed);
  for (int ch = 0; ch < 2; ++ch)
    g_ip[ch]->process(&in[ch * frames], &out[ch * outFrames], outFrames, g_speed, 0 /* 上次未读完的相位，交给 JUCE 保持 */);
  return outFrames;
}
int resample_outRate() { return g_outRate; }
}
#endif
