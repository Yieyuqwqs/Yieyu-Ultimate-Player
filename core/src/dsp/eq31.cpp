// [17] 31 段 EQ：ISO 1/3 倍频程，biquad 级联。
// 输出 31 个 dB 点给模块 2（Rive 曲线），这条通路是"模块 17 → 24 JSB → 2 Rive"。
#include "../common/log.h"
#include <cmath>

#ifndef YIEYU_PI
#define YIEYU_PI 3.14159265358979323846f
#endif
#include <cstring>

namespace yieyu {
namespace {
constexpr int kBands = 31;
constexpr float kFreqs[kBands] = {20,25,31.5f,40,50,63,80,100,125,160,200,250,315,400,500,630,800,
                                  1000,1250,1600,2000,2500,3150,4000,5000,6300,8000,10000,12500,16000,20000};
struct Biquad { float b0,b1,b2,a1,a2,z1,z2; };
Biquad g_bands[kBands];
float  g_gainDb[kBands] = {0};
float  g_outDb[kBands]  = {0};
}  // namespace

void eq31_init(int sampleRate) {
  for (int i = 0; i < kBands; ++i) {
    const float w0 = 2.0f * YIEYU_PI * kFreqs[i] / (float)sampleRate;
    const float alpha = std::sin(w0) / (2.0f * 1.41f);  // Q≈1.41 → 1/3 倍频程
    const float a0 = 1.0f + alpha;
    g_bands[i] = { alpha / a0, 0.0f, -alpha / a0, -2.0f * std::cos(w0) / a0, (1.0f - alpha) / a0, 0, 0 };
  }
  LOGI("[17] EQ31 初始化 @%d Hz（%d 段）", sampleRate, kBands);
}

void eq31_set_gain(int band, float db) {
  if (band < 0 || band >= kBands) return;
  g_gainDb[band] = db;
  const float A = std::pow(10.0f, db / 40.0f);
  const float w0 = 2.0f * YIEYU_PI * kFreqs[band] / 48000.0f;
  const float alpha = std::sin(w0) / (2.0f * 1.41f);
  const float a0 = 1.0f + alpha / A;
  g_bands[band].b0 = (1.0f + alpha * A) / a0;
  g_bands[band].b1 = (-2.0f * std::cos(w0)) / a0;
  g_bands[band].b2 = (1.0f - alpha * A) / a0;
  g_bands[band].a1 = (-2.0f * std::cos(w0)) / a0;
  g_bands[band].a2 = (1.0f - alpha / A) / a0;
}

// 立体声就地处理
void eq31_process(float* interleaved, int frames) {
  for (int i = 0; i < kBands; ++i) {
    if (g_gainDb[i] == 0.0f) continue;
    Biquad& q = g_bands[i];
    for (int n = 0; n < frames; ++n) {
      for (int ch = 0; ch < 2; ++ch) {
        float& x = interleaved[n * 2 + ch];
        const float y = q.b0 * x + q.z1;
        q.z1 = q.b1 * x - q.a1 * y + q.z2;
        q.z2 = q.b2 * x - q.a2 * y;
        x = y;
      }
    }
  }
}

// 给 Rive 的 31 点（曲线跳动）
int eq31_points(float* outDb) {
  std::memcpy(outDb, g_outDb, sizeof(float) * kBands);
  return kBands;
}
}  // namespace yieyu
