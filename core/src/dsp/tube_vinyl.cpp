// [17] 胆机软削波 + 黑胶味精。
// 血泪教训（继承自 Aurora-30）：蓝牙耳机下 16k 以上能量会顶出"滋滋"，所以输出端常驻 16k 陡降 LPF。
#include "../common/log.h"
#include <cmath>
#include <cstdio>
#include <initializer_list>   // for (Lpf* f : {&g_lpfL, &g_lpfR}) 需要它

#ifndef YIEYU_PI
#define YIEYU_PI 3.14159265358979323846f
#endif

namespace yieyu {
namespace {
bool  g_tube = true, g_vinyl = false, g_lpf = true;
float g_drive = 1.6f;
float g_lpfCutoff = 16000.0f;   // 嫌闷就抬到 17500
struct Lpf { float z1 = 0, z2 = 0, a1 = 0, a2 = 0, b0 = 0; };
Lpf g_lpfL, g_lpfR;
double g_wowPhase = 0;
}  // namespace

void tube_vinyl_init(int sampleRate) {
  // 12 阶级联巴特沃斯（骨架期先二阶占位，P2 补级联）
  const float w0 = 2.0f * YIEYU_PI * g_lpfCutoff / (float)sampleRate;
  const float alpha = std::sin(w0) / (2.0f * 0.7071f);
  const float a0 = 1.0f + alpha;
  const float b0 = (1.0f - std::cos(w0)) / 2.0f / a0;
  for (Lpf* f : {&g_lpfL, &g_lpfR}) { f->b0 = b0; f->a1 = (-2.0f * std::cos(w0)) / a0; f->a2 = (1.0f - alpha) / a0; }
  LOGI("[17] tube=%d vinyl=%d lpf=%d @%.0fHz", g_tube, g_vinyl, g_lpf, g_lpfCutoff);
}

void tube_vinyl_process(float* interleaved, int frames) {
  for (int n = 0; n < frames; ++n) {
    for (int ch = 0; ch < 2; ++ch) {
      float x = interleaved[n * 2 + ch];
      if (g_tube)  x = std::tanh(x * g_drive) / std::tanh(g_drive);   // 软削波，无硬拐点
      if (g_vinyl) { /* TODO(P3)：wow/flutter 相位调制 + 纹噪混入 + RIAA */ }
      if (g_lpf) {
        Lpf& f = ch == 0 ? g_lpfL : g_lpfR;
        const float y = f.b0 * x + f.z1;
        f.z1 = -f.a1 * y + f.z2;
        f.z2 = -f.a2 * y + f.b0 * x;   // 简化直 I 型转置
        x = y;
      }
      interleaved[n * 2 + ch] = x;
    }
  }
}

void tube_vinyl_set(bool tube, bool vinyl, bool lpf, float drive) {
  g_tube = tube; g_vinyl = vinyl; g_lpf = lpf;
  if (drive > 0.0f && std::isfinite(drive)) g_drive = drive;   // 非法值回落，不炸
}
}  // namespace yieyu
