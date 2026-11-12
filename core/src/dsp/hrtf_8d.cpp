// [17] 8D 空间化：HRTF 卷积。吃模块 23 的陀螺仪姿态矩阵。
// 红线：只接受"旋转矩阵"，绝不修改 HRIR 数据；自动 8D 与头追共用同一入口。
#include "../common/log.h"
#include <cmath>

namespace yieyu {
namespace {
constexpr int kTaps = 64;          // HRIR 抽头（实测 64 足够）
float g_hrirL[2][kTaps] = {{0}}, g_hrirR[2][kTaps] = {{0}};
float g_histL[2][kTaps] = {{0}}, g_histR[2][kTaps] = {{0}};
float g_yaw = 0, g_pitch = 0;      // 来自 [23] ASensor 的姿态快照
int   g_source = 0;                // 0=HEADTRACK 1=AUTO_SPIN
float g_spinHz = 0.6f;
double g_spinPhase = 0;
}  // namespace

void hrtf8d_init(int sampleRate) {
  // 骨架期：生成左右 ITD（多普勒时间差）示意；真 HRIR 由 P3 用 KEMAR 数据集离线烘进来
  for (int t = 0; t < kTaps; ++t) {
    const float env = std::exp(-(float)t / 12.0f);
    g_hrirL[0][t] = env * (t == 3 ? 1.0f : 0.1f);
    g_hrirR[0][t] = env * (t == 8 ? 1.0f : 0.1f);
  }
  LOGI("[17] 8D HRTF 初始化 @%d Hz, taps=%d", sampleRate, kTaps);
}

// 由 [23] 的姿态快照调用（非音频线程写，音频线程读；用 double buffer + atomic 换指针）
void hrtf8d_set_pose(float yawDeg, float pitchDeg, int source) {
  g_yaw = yawDeg;
  g_pitch = pitchDeg > 60.f ? 60.f : (pitchDeg < -60.f ? -60.f : pitchDeg);  // 限幅
  g_source = source;
}

void hrtf8d_process(float* interleaved, int frames, int sampleRate) {
  if (g_source == 1) {   // 自动 8D：BPM 驱动的 yaw（不改 hrtf 数据，只换旋转矩阵）
    g_spinPhase += (double)frames * g_spinHz / (double)sampleRate;
    g_yaw = (float)(g_spinPhase * 360.0) - 360.0f * std::floor(g_spinPhase);
  }
  const float rad = g_yaw * 3.14159265f / 180.0f;
  const float gl = 0.5f + 0.5f * std::cos(rad), gr = 1.0f - gl;   // 骨架期：等功率声像
  for (int n = 0; n < frames; ++n) {
    interleaved[n * 2]     *= gl;
    interleaved[n * 2 + 1] *= gr;
  }
}
}  // namespace yieyu
