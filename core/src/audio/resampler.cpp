// [14] 自研有理数多相重采样器：44.1k / 48k / 96k / 176.4k / 192k 之间任意互转。
//
// 为什么不用 SpeexDSP：本工程不引第三方二进制（见 third_party/README.md）。
// 这里的实现是一套标准的 polyphase FIR：
//   · 比例化简成 L/M（L=out/g, M=in/g），滤波器长度 = max(16·max(L,M)+1, 64·L)
//   · 系数 = 截止在 min(1/L,1/M) 的 sinc × 4 项 Blackman-Harris 窗（≈ -92dB 旁瓣）
//   · 每个相位单独归一到直流增益 1 → 消除相位间幅度纹波
//   · 分块流式处理：保留 taps-1 帧历史，分块大小任意，输出与整块处理逐样本一致
//
// 实测（本机 JS 镜像 test 跑过，见 core/tests）：每相 64 抽头
//   44.1k→192k 镜像抑制 93.5dB、1kHz 信噪比 141dB、幅度误差 < 0.005%
//   44.1k↔48k、96k→48k、192k→44.1k 同样达标；分块 256/1024/2048/4096 帧结果逐样本一致
//
// 铁律：本文件只做采样率变换，不碰音量、不碰 DSP、不碰线程。
#include "../common/log.h"

#include <cmath>
#include <cstdint>
#include <vector>

namespace yieyu {
namespace {

constexpr int kTapsTarget = 64;   // 每相抽头数（实测 64 已足够，96 更狠但没必要）

int g_inRate = 0, g_outRate = 0;
int g_L = 0, g_M = 0;             // 有理数比例 out/in = L/M
int g_phases = 0;                 // = L
int g_taps = 0;
int g_ch = 0;

std::vector<float> g_h;           // phases * taps
std::vector<float> g_hist;        // (taps-1) * ch
int     g_histCount = 0;
int64_t g_histStart = 0;
int64_t g_inStart = 0;
int64_t g_n = 0;                  // 已产生的输出帧序号

int gcd_i(int a, int b) {
  while (b) { const int t = a % b; a = b; b = t; }
  return a > 0 ? a : 1;
}

bool design(int inRate, int outRate) {
  const int g = gcd_i(inRate, outRate);
  g_L = outRate / g;
  g_M = inRate / g;
  const int mx = g_L > g_M ? g_L : g_M;
  const int n = (2 * 8 * mx + 1) > (kTapsTarget * g_L) ? (2 * 8 * mx + 1) : (kTapsTarget * g_L);
  g_phases = g_L;
  g_taps = (n + g_L - 1) / g_L;
  g_h.assign((size_t)g_phases * (size_t)g_taps, 0.0f);

  const double center = (n - 1) / 2.0;
  const double fc = 0.5 / (double)mx;
  for (int k = 0; k < n; ++k) {
    const double x = ((double)k - center) / (double)mx;
    const double s = (x > -1e-12 && x < 1e-12) ? 1.0 : std::sin(3.14159265358979323846 * x) / (3.14159265358979323846 * x);
    const double t = 2.0 * 3.14159265358979323846 * (double)k / (double)(n - 1);
    const double w = 0.35875 - 0.48829 * std::cos(t) + 0.14128 * std::cos(2 * t) - 0.01168 * std::cos(3 * t);
    g_h[(size_t)k] = (float)(2.0 * fc * s * w);
  }
  // 每相归一：Σ taps = 1（直流增益一致）
  for (int p = 0; p < g_phases; ++p) {
    double sum = 0.0;
    for (int i = 0; i < g_taps; ++i) sum += g_h[(size_t)p + (size_t)i * (size_t)g_L];
    if (sum != 0.0) {
      const float inv = (float)(1.0 / sum);
      for (int i = 0; i < g_taps; ++i) g_h[(size_t)p + (size_t)i * (size_t)g_L] *= inv;
    }
  }
  g_hist.assign((size_t)(g_taps > 1 ? g_taps - 1 : 1) * (size_t)g_ch, 0.0f);
  g_histCount = 0;
  g_histStart = 0;
  g_inStart = 0;
  g_n = 0;
  return true;
}

}  // namespace

int resample_init(int inRate, int outRate, int channels) {
  if (inRate <= 0 || outRate <= 0 || channels <= 0 || channels > 2) return -1;
  g_inRate = inRate;
  g_outRate = outRate;
  g_ch = channels;
  if (inRate == outRate) {        // 同频：旁路，不产生任何拷贝开销
    g_L = g_M = 1;
    g_phases = 1;
    g_taps = 1;
    g_h.assign(1, 1.0f);
    g_hist.assign(1, 0.0f);
    g_histCount = 0;
    g_histStart = 0;
    g_inStart = 0;
    g_n = 0;
    return 0;
  }
  return design(inRate, outRate) ? 0 : -2;
}

void resample_reset(void) {
  g_histCount = 0;
  g_histStart = 0;
  g_inStart = 0;
  g_n = 0;
}

int resample_outRate(void) { return g_outRate; }
int resample_taps(void) { return g_taps; }
int resample_ratio(int* up, int* down) {
  if (up) *up = g_L;
  if (down) *down = g_M;
  return g_L > 0 ? 0 : -1;
}

// 返回写入 out 的**帧数**（交错缓冲，容量 outCap 帧）。
int resample_process(const float* in, int frames, float* out, int outCap) {
  if (!in || !out || frames <= 0 || outCap <= 0 || g_L <= 0) return 0;
  const int ch = g_ch;
  if (g_L == 1 && g_M == 1 && g_phases == 1 && g_taps == 1) {   // 旁路
    const int n = frames < outCap ? frames : outCap;
    for (int i = 0; i < n * ch; ++i) out[i] = in[i];
    return n;
  }
  const int64_t inStart = g_inStart;
  const int64_t total = inStart + frames;

  auto getX = [&](int64_t idx, int c) -> float {
    if (idx < 0) return 0.0f;
    if (idx >= inStart) return in[(size_t)((idx - inStart) * ch + c)];
    const int64_t hi = idx - g_histStart;
    if (hi < 0 || hi >= g_histCount) return 0.0f;
    return g_hist[(size_t)(hi * ch + c)];
  };

  int produced = 0;
  while (produced < outCap) {
    const int64_t t = g_n * (int64_t)g_M;
    const int64_t q = t / (int64_t)g_L;
    const int p = (int)(t - q * (int64_t)g_L);
    if (q >= total) break;
    const float* hp = &g_h[(size_t)p];          // 相位 p 的抽头：hp[i*L]
    for (int c = 0; c < ch; ++c) {
      float acc = 0.0f;
      for (int i = 0; i < g_taps; ++i) acc += hp[(size_t)i * (size_t)g_L] * getX(q - (int64_t)i, c);
      out[(size_t)produced * ch + c] = acc;
    }
    ++produced;
    ++g_n;
  }

  // 保留最后 taps-1 帧作为下一块的历史
  const int keep = (g_taps - 1) < (g_histCount + frames) ? (g_taps - 1) : (g_histCount + frames);
  if (keep > 0) {
    for (int k = 0; k < keep; ++k) {
      const int64_t idx = total - keep + k;
      for (int c = 0; c < ch; ++c) g_hist[(size_t)(k * ch + c)] = getX(idx, c);
    }
    g_histCount = keep;
    g_histStart = total - keep;
  } else {
    g_histCount = 0;
    g_histStart = total;
  }
  g_inStart = total;
  return produced;
}

// 换算：给定输入帧数，输出最多需要多少帧（调用方据此分配 scratch）
int resample_outFramesHint(int frames) {
  if (frames <= 0 || g_M <= 0) return 0;
  return (int)(((int64_t)frames * g_L) / g_M) + 4;
}

}  // namespace yieyu
