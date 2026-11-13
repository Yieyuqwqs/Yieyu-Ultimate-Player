// [18] 离线人声消除实现：WAV 读写 + 自研 radix-2 FFT + STFT 中置抑制 + 重叠相加。
//
// 数学：对一个立体声帧做 STFT，对每个频点
//   mid = (L+R)/2, side = (L-R)/2
//   mask = |mid|^p / (|mid|^p + |side|^p + eps)      // 中置能量占比
//   L' = L - kCenterAtten · w(f) · mask · mid
//   R' = R - kCenterAtten · w(f) · mask · mid
// 人声在混音里通常居中 → 被 mask 选中压掉；贝斯/鼓/吉他往往有边分量 → 保留。
// 合成用 Hann 分析 + Hann 合成、hop=N/4（w² 的和为 1.5，输出除以 1.5）。
//
// 铁律：本文件只做离线处理，不碰声卡线程；出错只写黑匣子。
#include "vocals.h"
#include "vocal_kernel.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "../common/log.h"

namespace {

char g_err[192] = {0};
void set_err(const char* s) { std::snprintf(g_err, sizeof(g_err), "%s", s); }

// ── 自研 radix-2 FFT（原地，长度必须是 2 的幂）─────────────────────
void fft(float* re, float* im, int n, bool inverse) {
  // 位反转置换
  for (int i = 1, j = 0; i < n; ++i) {
    int bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) {
      float t = re[i]; re[i] = re[j]; re[j] = t;
      t = im[i]; im[i] = im[j]; im[j] = t;
    }
  }
  for (int len = 2; len <= n; len <<= 1) {
    const double ang = 2.0 * 3.14159265358979323846 / (double)len * (inverse ? 1.0 : -1.0);
    const float wr = (float)std::cos(ang), wi = (float)std::sin(ang);
    for (int i = 0; i < n; i += len) {
      float cr = 1.0f, ci = 0.0f;
      for (int k = 0; k < len / 2; ++k) {
        const int a = i + k, b = i + k + len / 2;
        const float xr = re[b] * cr - im[b] * ci;
        const float xi = re[b] * ci + im[b] * cr;
        re[b] = re[a] - xr; im[b] = im[a] - xi;
        re[a] += xr;        im[a] += xi;
        const float nr = cr * wr - ci * wi;
        ci = cr * wi + ci * wr;
        cr = nr;
      }
    }
  }
  if (inverse) {
    const float inv = 1.0f / (float)n;
    for (int i = 0; i < n; ++i) { re[i] *= inv; im[i] *= inv; }
  }
}

uint32_t rd_u32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
uint16_t rd_u16(const uint8_t* p) { return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8)); }
void wr_u32(std::vector<uint8_t>& v, uint32_t x) { for (int i = 0; i < 4; ++i) v.push_back((uint8_t)((x >> (8 * i)) & 0xFF)); }
void wr_u16(std::vector<uint8_t>& v, uint16_t x) { for (int i = 0; i < 2; ++i) v.push_back((uint8_t)((x >> (8 * i)) & 0xFF)); }

struct Wav {
  int32_t rate = 0;
  int32_t ch = 0;
  int32_t bits = 0;
  int32_t fmt = 1;              // 1=PCM 3=float
  std::vector<float> l, r;      // 拆分后的双声道（单声道会复制到两边）
  int32_t frames = 0;
};

bool read_wav(const char* path, Wav* out) {
  FILE* f = std::fopen(path, "rb");
  if (!f) { set_err("打不开输入 WAV"); return false; }
  std::fseek(f, 0, SEEK_END);
  const long size = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  if (size < 44) { std::fclose(f); set_err("WAV 文件太小"); return false; }
  std::vector<uint8_t> buf((size_t)size);
  if (std::fread(buf.data(), 1, buf.size(), f) != buf.size()) { std::fclose(f); set_err("WAV 读不满"); return false; }
  std::fclose(f);
  if (std::memcmp(buf.data(), "RIFF", 4) != 0 || std::memcmp(buf.data() + 8, "WAVE", 4) != 0) {
    set_err("不是 WAV（RIFF/WAVE）");
    return false;
  }
  size_t off = 12;
  size_t dataOff = 0, dataLen = 0;
  int32_t bits = 0, ch = 0, fmtTag = 0, rate = 0;
  while (off + 8 <= buf.size()) {
    const uint32_t csize = rd_u32(&buf[off + 4]);
    if (std::memcmp(&buf[off], "fmt ", 4) == 0 && off + 8 + 16 <= buf.size()) {
      fmtTag = rd_u16(&buf[off + 8]);
      ch = rd_u16(&buf[off + 10]);
      rate = (int32_t)rd_u32(&buf[off + 12]);
      bits = rd_u16(&buf[off + 22]);
      if (fmtTag == 0xFFFE) fmtTag = 1;          // WAVE_FORMAT_EXTENSIBLE 当 PCM
    } else if (std::memcmp(&buf[off], "data", 4) == 0) {
      dataOff = off + 8;
      dataLen = csize;
      if (dataOff + dataLen > buf.size()) dataLen = buf.size() - dataOff;
    }
    off += 8 + (size_t)csize + (csize & 1u);
  }
  if (!dataOff || !ch || !rate) { set_err("WAV 缺 fmt/data 块"); return false; }
  if (ch > 2) { set_err("只支持单/双声道 WAV"); return false; }
  if (bits != 16 && bits != 24 && bits != 32) { set_err("只支持 16/24/32 位 WAV"); return false; }

  const int bytesPer = bits / 8;
  const size_t oneFrame = (size_t)bytesPer * (size_t)ch;
  const int32_t frames = (int32_t)(dataLen / oneFrame);
  out->rate = rate;
  out->ch = ch;
  out->bits = bits;
  out->fmt = fmtTag;
  out->frames = frames;
  out->l.resize((size_t)frames);
  out->r.resize((size_t)frames);
  const uint8_t* p = buf.data() + dataOff;
  for (int32_t i = 0; i < frames; ++i) {
    float v[2] = {0.0f, 0.0f};
    for (int c = 0; c < ch; ++c) {
      const uint8_t* q = p + (size_t)i * oneFrame + (size_t)c * bytesPer;
      float s = 0.0f;
      if (fmtTag == 3 && bits == 32) {
        float fv = 0.0f;
        std::memcpy(&fv, q, 4);
        s = fv;
      } else if (bits == 16) {
        s = (float)(int16_t)((uint16_t)q[0] | ((uint16_t)q[1] << 8)) / 32768.0f;
      } else if (bits == 24) {
        int32_t x = (int32_t)q[0] | ((int32_t)q[1] << 8) | ((int32_t)q[2] << 16);
        if (x & 0x800000) x |= ~0xFFFFFF;
        s = (float)x / 8388608.0f;
      } else {
        int32_t x = 0;
        std::memcpy(&x, q, 4);
        s = (float)x / 2147483648.0f;
      }
      v[c] = s;
    }
    out->l[(size_t)i] = v[0];
    out->r[(size_t)i] = (ch == 2) ? v[1] : v[0];
  }
  return true;
}

bool write_wav16(const char* path, int32_t rate, const std::vector<float>& l, const std::vector<float>& r) {
  const int32_t frames = (int32_t)l.size();
  const uint32_t dataLen = (uint32_t)frames * 4u;
  std::vector<uint8_t> hdr;
  hdr.reserve(44);
  hdr.push_back('R'); hdr.push_back('I'); hdr.push_back('F'); hdr.push_back('F');
  wr_u32(hdr, 36u + dataLen);
  hdr.push_back('W'); hdr.push_back('A'); hdr.push_back('V'); hdr.push_back('E');
  hdr.push_back('f'); hdr.push_back('m'); hdr.push_back('t'); hdr.push_back(' ');
  wr_u32(hdr, 16);
  wr_u16(hdr, 1);                     // PCM
  wr_u16(hdr, 2);                     // 立体声
  wr_u32(hdr, (uint32_t)rate);
  wr_u32(hdr, (uint32_t)rate * 4u);
  wr_u16(hdr, 4);
  wr_u16(hdr, 16);
  hdr.push_back('d'); hdr.push_back('a'); hdr.push_back('t'); hdr.push_back('a');
  wr_u32(hdr, dataLen);

  FILE* f = std::fopen(path, "wb");
  if (!f) { set_err("写不了输出 WAV"); return false; }
  if (std::fwrite(hdr.data(), 1, hdr.size(), f) != hdr.size()) { std::fclose(f); set_err("写 WAV 头失败"); return false; }
  std::vector<int16_t> block((size_t)frames * 2);
  for (int32_t i = 0; i < frames; ++i) {
    float a = l[(size_t)i], b = r[(size_t)i];
    if (a > 1.0f) a = 1.0f;
    if (a < -1.0f) a = -1.0f;
    if (b > 1.0f) b = 1.0f;
    if (b < -1.0f) b = -1.0f;
    block[(size_t)i * 2] = (int16_t)std::lrint(a * 32767.0f);
    block[(size_t)i * 2 + 1] = (int16_t)std::lrint(b * 32767.0f);
  }
  const size_t want = block.size() * sizeof(int16_t);
  const size_t wrote = std::fwrite(block.data(), 1, want, f);
  std::fclose(f);
  if (wrote != want) { set_err("写 WAV 数据失败"); return false; }
  return true;
}

}  // namespace

extern "C" {

const char* yieyu_vocals_error(void) { return g_err; }

int yieyu_vocals_render(const char* srcWav, const char* dstWav, void (*onProgress)(float)) {
  g_err[0] = 0;
  if (!srcWav || !dstWav || !*srcWav || !*dstWav) return -1;
  Wav w;
  if (!read_wav(srcWav, &w)) return -2;
  if (w.frames < yieyu::vocal::kNfft * 2) { set_err("音频太短（< 2 个 STFT 窗）"); return -3; }

  const int N = yieyu::vocal::kNfft;
  const int hop = yieyu::vocal::kHop;
  const int bins = yieyu::vocal::kBins;
  const float* win = yieyu::vocal::kWindow;
  const float* shape = yieyu::vocal::kWeightShape;

  std::vector<float> outL((size_t)w.frames + (size_t)N, 0.0f);
  std::vector<float> outR((size_t)w.frames + (size_t)N, 0.0f);
  std::vector<float> reL((size_t)N), imL((size_t)N), reR((size_t)N), imR((size_t)N);

  const int totalFrames = (int)((w.frames + hop - 1) / hop) + 1;
  int done = 0;
  for (int start = -N + hop; start < w.frames; start += hop) {
    // 分析（窗口外补零）
    for (int i = 0; i < N; ++i) {
      const int idx = start + i;
      const float a = (idx >= 0 && idx < w.frames) ? w.l[(size_t)idx] * win[i] : 0.0f;
      const float b = (idx >= 0 && idx < w.frames) ? w.r[(size_t)idx] * win[i] : 0.0f;
      reL[(size_t)i] = a; imL[(size_t)i] = 0.0f;
      reR[(size_t)i] = b; imR[(size_t)i] = 0.0f;
    }
    fft(reL.data(), imL.data(), N, false);
    fft(reR.data(), imR.data(), N, false);

    for (int k = 0; k < bins; ++k) {
      const float lr = reL[(size_t)k], li = imL[(size_t)k];
      const float rr = reR[(size_t)k], ri = imR[(size_t)k];
      const float midR = 0.5f * (lr + rr), midI = 0.5f * (li + ri);
      const float sideR = 0.5f * (lr - rr), sideI = 0.5f * (li - ri);
      const float mp = midR * midR + midI * midI;
      const float sp = sideR * sideR + sideI * sideI;
      const float mask = mp / (mp + sp + yieyu::vocal::kEps);
      // 频点 → 权重曲线索引（design.py 以 1kHz 为尺度）
      int wi = (int)((float)k * (float)w.rate * (float)bins / ((float)N * 1000.0f));
      if (wi < 0) wi = 0;
      if (wi >= bins) wi = bins - 1;
      const float atten = yieyu::vocal::kCenterAtten * shape[wi] * mask;
      reL[(size_t)k] = lr - atten * midR;
      imL[(size_t)k] = li - atten * midI;
      reR[(size_t)k] = rr - atten * midR;
      imR[(size_t)k] = ri - atten * midI;
      // 共轭对称：N/2+1..N-1 由 1..N/2-1 镜像得到（保证 IFFT 出实信号）
      if (k > 0 && k < N / 2) {
        const int m = N - k;
        reL[(size_t)m] = reL[(size_t)k];
        imL[(size_t)m] = -imL[(size_t)k];
        reR[(size_t)m] = reR[(size_t)k];
        imR[(size_t)m] = -imR[(size_t)k];
      }
    }

    fft(reL.data(), imL.data(), N, true);
    fft(reR.data(), imR.data(), N, true);

    // 重叠相加（合成窗 = Hann，w² 的总和 = 1.5，最后整体除以 1.5）
    for (int i = 0; i < N; ++i) {
      const int idx = start + i;
      if (idx < 0) continue;
      if (idx >= w.frames) break;
      outL[(size_t)idx] += reL[(size_t)i] * win[i];
      outR[(size_t)idx] += reR[(size_t)i] * win[i];
    }
    ++done;
    if (onProgress && totalFrames > 0) onProgress((float)done / (float)totalFrames);
  }

  const float norm = 1.0f / 1.5f;
  for (int32_t i = 0; i < w.frames; ++i) {
    outL[(size_t)i] *= norm;
    outR[(size_t)i] *= norm;
  }
  outL.resize((size_t)w.frames);
  outR.resize((size_t)w.frames);
  if (!write_wav16(dstWav, w.rate, outL, outR)) return -4;
  if (onProgress) onProgress(1.0f);
  return 0;
}

}  // extern "C"
