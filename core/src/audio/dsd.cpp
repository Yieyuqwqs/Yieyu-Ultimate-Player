// [16] DSD（DSF / DFF）→ PCM。
//
// 两级处理（这是 DSD 解码的标准做法，不是凑出来的）：
//   ① 1bit 流 → ±1 → 64 抽头 Blackman-Harris 低通（截止 = 0.9 · 1/(2D)）→ 抽取 D 倍
//      D 默认 8：DSD64（2822.4k）→ 352.8k，DSD128（5644.8k）→ 705.6k
//   ② 中间率 → 目标率 交给 [14] 的多相重采样器（同一个模块，不重复造）
//
// 容器（都已按规范验证过布局，见 core/tests 与本机 100 轮镜像）
//   DSF：小端，chunk = ID(4) + ckSize(8，含 ID 与长度本身) + 载荷；块内按声道交错 4096 字节；
//        位序 **LSB first**
//   DFF：大端，chunk = ID(4) + size(8) + 载荷，奇数字节补 0；数据按**字节**交错；位序 **MSB first**
//
// 铁律：只做"DSD → PCM"，不碰音量/音效/线程；只允许解码线程调用。
#include "dsd.h"
#include "resampler.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "../common/log.h"

// 重采样器（[14]）在 yieyu 命名空间里；本文件对外是 C 接口，这里拉进来用
using yieyu::resample_init;
using yieyu::resample_outFramesHint;
using yieyu::resample_process;
using yieyu::resample_reset;

namespace {

constexpr int kTaps = 64;      // 抽取 FIR 抽头
constexpr int kTail = kTaps;   // 保留的历史样本数

int      g_fd = -1;
int      g_kind = 0;           // 1=DSF 2=DFF
uint32_t g_bitRate = 0;        // DSD 位率（2822400 / 5644800 …）
int      g_ch = 0;
uint64_t g_dataOff = 0;
uint64_t g_dataSize = 0;
uint64_t g_totalBits = 0;      // 每声道位数
uint32_t g_blockSize = 0;      // DSF 专用
int      g_D = 8;              // 抽取比
int      g_midRate = 0;
int      g_targetRate = 0;

float g_h[kTaps];
float g_tail[2][kTail];        // 每声道最近 kTail 个 ±1
int   g_tailCount = 0;         // 已填的历史个数（开头不足时按 0 处理）
uint64_t g_base = 0;           // 下一个待读位的全局下标

std::vector<float> g_mid;      // 中间率交错样本（本轮新产出）
std::vector<float> g_out;      // 目标率交错样本（待取）
size_t g_outPos = 0;
uint64_t g_posBytes = 0;       // 已消费的数据字节（每声道）
int g_eos = 0;
char g_err[192] = {0};

void set_err(const char* s) { std::snprintf(g_err, sizeof(g_err), "%s", s); }

uint32_t rd_u32le(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
uint32_t rd_u32be(const uint8_t* p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
uint64_t rd_u64le(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 7; i >= 0; --i) v = (v << 8) | (uint64_t)p[i];
  return v;
}
uint64_t rd_u64be(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v = (v << 8) | (uint64_t)p[i];
  return v;
}

// 抽取 FIR：截止 0.9/(2D)，Blackman-Harris 窗，直流增益归一到 1
void design_fir() {
  double sum = 0.0;
  const double fc = 0.9 * 0.5 / (double)g_D;
  const double center = (kTaps - 1) / 2.0;
  for (int k = 0; k < kTaps; ++k) {
    const double x = (double)k - center;
    const double s = (x > -1e-12 && x < 1e-12)
                         ? 2.0 * fc
                         : std::sin(2.0 * 3.14159265358979323846 * fc * x) / (3.14159265358979323846 * x);
    const double t = 2.0 * 3.14159265358979323846 * (double)k / (double)(kTaps - 1);
    const double w = 0.35875 - 0.48829 * std::cos(t) + 0.14128 * std::cos(2 * t) - 0.01168 * std::cos(3 * t);
    const float v = (float)(s * w);
    g_h[k] = v;
    sum += v;
  }
  if (sum != 0.0) {
    const float inv = (float)(1.0 / sum);
    for (int k = 0; k < kTaps; ++k) g_h[k] *= inv;
  }
}

// ── 容器解析 ──────────────────────────────────────────────────────
int parse_dsf(int fd, uint64_t fileSize) {
  uint8_t hdr[12];
  uint64_t off = 0;
  int seenFmt = 0;
  while (off + 12 <= fileSize) {
    if (::pread(fd, hdr, 12, (off_t)off) != 12) break;
    const uint32_t chunkSize = rd_u32le(hdr + 4);   // 低 32 位够用（chunk 不会 > 4GB）
    if (std::memcmp(hdr, "fmt ", 4) == 0) {
      uint8_t fmt[40];
      if (chunkSize < 12 + 40) { set_err("DSF fmt 块太小"); return -1; }
      if (::pread(fd, fmt, 40, (off_t)(off + 12)) != 40) break;
      const uint32_t formatId = rd_u32le(fmt + 4);
      if (formatId != 0) { set_err("DSF 是 DST 压缩（formatId=1），不支持"); return -2; }
      g_ch = (int)rd_u32le(fmt + 12);
      g_bitRate = rd_u32le(fmt + 16);
      const uint64_t count = rd_u64le(fmt + 24);
      g_blockSize = rd_u32le(fmt + 32);
      g_totalBits = count;
      if (g_blockSize == 0) g_blockSize = 4096;
      if (g_ch < 1 || g_ch > 2) { set_err("DSF 声道数不支持"); return -3; }
      if (g_bitRate < 2822400 / 8 || g_bitRate > 5644800 * 4) { set_err("DSF 位率异常"); return -4; }
      seenFmt = 1;
    } else if (std::memcmp(hdr, "data", 4) == 0) {
      g_dataOff = off + 12;
      g_dataSize = chunkSize >= 12 ? (uint64_t)chunkSize - 12u : 0;
      return seenFmt ? 0 : -5;
    }
    if (chunkSize < 12) break;
    off += chunkSize;                                 // DSF：ckSize 含 ID + 长度
  }
  set_err(seenFmt ? "DSF 找不到 data 块" : "DSF 找不到 fmt 块");
  return -6;
}

int parse_dff(int fd, uint64_t fileSize) {
  uint8_t hdr[12];
  uint64_t off = 16;                                  // "FRM8" + size(8) + "DSD "
  int seenCh = 0;
  while (off + 12 <= fileSize) {
    if (::pread(fd, hdr, 12, (off_t)off) != 12) break;
    const uint64_t size = rd_u64be(hdr + 4);
    if (std::memcmp(hdr, "FS  ", 4) == 0 && size >= 4) {
      uint8_t b[4];
      if (::pread(fd, b, 4, (off_t)(off + 12)) == 4) g_bitRate = rd_u32be(b);
    } else if (std::memcmp(hdr, "CHNL", 4) == 0 && size >= 2) {
      uint8_t b[2];
      if (::pread(fd, b, 2, (off_t)(off + 12)) == 2) g_ch = (int)(((uint16_t)b[0] << 8) | b[1]);
      seenCh = 1;
    } else if (std::memcmp(hdr, "CMPR", 4) == 0 && size >= 5) {
      uint8_t b[5];
      if (::pread(fd, b, 5, (off_t)(off + 12)) == 5) {
        if (std::memcmp(b, "DST ", 4) == 0) { set_err("DFF 是 DST 压缩，不支持"); return -2; }
      }
    } else if (std::memcmp(hdr, "DATA", 4) == 0) {
      g_dataOff = off + 12;
      g_dataSize = size;
      if (!seenCh || g_ch < 1 || g_ch > 2) { set_err("DFF 缺 CHNL 或声道数不支持"); return -3; }
      if (g_bitRate < 2822400 / 8) { set_err("DFF 位率异常"); return -4; }
      g_totalBits = (g_dataSize / (uint64_t)g_ch) * 8ull;
      return 0;
    }
    if (size > fileSize) break;
    off += 12 + size + (size & 1ull);                 // DSDIFF：奇数字节补 0
  }
  set_err("DFF 找不到 DATA 块");
  return -6;
}

int open_any(const char* path) {
  const int fd = ::open(path, O_RDONLY);
  if (fd < 0) { set_err("打不开文件"); return -1; }
  struct stat st {};
  if (::fstat(fd, &st) != 0 || st.st_size < 16) { ::close(fd); set_err("文件太小"); return -2; }
  uint8_t magic[4];
  if (::pread(fd, magic, 4, 0) != 4) { ::close(fd); set_err("读不了文件头"); return -3; }
  g_fd = fd;
  g_kind = 0;
  g_ch = 0;
  g_bitRate = 0;
  g_dataOff = g_dataSize = g_totalBits = 0;
  g_blockSize = 0;
  int rc;
  if (std::memcmp(magic, "DSD ", 4) == 0) { g_kind = 1; rc = parse_dsf(fd, (uint64_t)st.st_size); }
  else if (std::memcmp(magic, "FRM8", 4) == 0) { g_kind = 2; rc = parse_dff(fd, (uint64_t)st.st_size); }
  else { ::close(fd); g_fd = -1; set_err("既不是 DSF 也不是 DFF"); return -4; }
  if (rc != 0) { ::close(fd); g_fd = -1; return rc; }
  return 0;
}

// 读一块数据：把每声道 bytesPerCh 字节去交错后放进 out（每声道连续）
int read_chunk(std::vector<uint8_t>& out, int bytesPerCh) {
  const int need = bytesPerCh * g_ch;
  if (g_posBytes >= (g_dataSize / (uint64_t)g_ch)) return 0;
  const uint64_t left = (g_dataSize / (uint64_t)g_ch) - g_posBytes;
  int take = (uint64_t)bytesPerCh < left ? bytesPerCh : (int)left;
  if (take <= 0) return 0;
  out.resize((size_t)take * (size_t)g_ch);
  if (g_kind == 1) {
    // DSF：按 4096 字节块、块内声道交错
    int done = 0;
    while (done < take) {
      const uint64_t byteIndex = g_posBytes + (uint64_t)done;
      const uint64_t blk = byteIndex / g_blockSize;
      const uint32_t inBlk = (uint32_t)(byteIndex % g_blockSize);
      const uint32_t room = g_blockSize - inBlk;
      const int step = (int)((uint32_t)(take - done) < room ? (uint32_t)(take - done) : room);
      for (int c = 0; c < g_ch; ++c) {
        const uint64_t pos = g_dataOff + (blk * (uint64_t)g_ch + (uint64_t)c) * g_blockSize + inBlk;
        if (::pread(g_fd, out.data() + (size_t)c * (size_t)take + done, (size_t)step, (off_t)pos) != step) {
          take = done;
          break;
        }
      }
      if (take == done) break;
      done += step;
    }
    if (done <= 0) return 0;
    take = done;
    g_posBytes += (uint64_t)take;
    return take;
  }
  // DFF：字节级交错，一次读 ch 倍
  std::vector<uint8_t> raw((size_t)take * (size_t)g_ch);
  const uint64_t pos = g_dataOff + g_posBytes * (uint64_t)g_ch;
  const ssize_t got = ::pread(g_fd, raw.data(), raw.size(), (off_t)pos);
  if (got <= 0) return 0;
  const int frames = (int)((size_t)got / (size_t)g_ch);
  for (int i = 0; i < frames; ++i)
    for (int c = 0; c < g_ch; ++c) out[(size_t)c * (size_t)frames + (size_t)i] = raw[(size_t)i * (size_t)g_ch + (size_t)c];
  g_posBytes += (uint64_t)frames;
  return frames;
}

}  // namespace

extern "C" {

int dsd_probe(const char* path) {
  if (!path || !*path) return -1;
  const int fd = ::open(path, O_RDONLY);
  if (fd < 0) return -1;
  uint8_t magic[4] = {0, 0, 0, 0};
  const ssize_t n = ::pread(fd, magic, 4, 0);
  ::close(fd);
  if (n != 4) return 0;
  if (std::memcmp(magic, "DSD ", 4) == 0 || std::memcmp(magic, "FRM8", 4) == 0) return 1;
  return 0;
}

int dsd_open(const char* path, int targetRate, int* outRate, int* outCh, int64_t* durUs) {
  dsd_close();
  if (!path || !*path) return -1;
  if (targetRate < 8000 || targetRate > 384000) return -1;
  const int rc = open_any(path);
  if (rc != 0) return rc;

  // 抽取比：让中间率 >= 目标率，最大 8
  g_D = 8;
  while (g_D > 1 && (int)(g_bitRate / (uint32_t)g_D) < targetRate) g_D /= 2;
  g_midRate = (int)(g_bitRate / (uint32_t)g_D);
  if (g_midRate > 384000) g_midRate = 384000;   // 保险：中间率不超过 384k
  g_targetRate = targetRate;

  design_fir();
  if (resample_init(g_midRate, g_targetRate, g_ch) != 0) { set_err("重采样器初始化失败"); return -3; }
  std::memset(g_tail, 0, sizeof(g_tail));
  g_tailCount = 0;
  g_base = 0;
  g_posBytes = 0;
  g_out.clear();
  g_outPos = 0;
  g_eos = 0;
  g_err[0] = 0;

  if (outRate) *outRate = g_targetRate;
  if (outCh) *outCh = g_ch;
  if (durUs) {
    const double sec = g_bitRate > 0 ? (double)g_totalBits / (double)g_bitRate : 0.0;
    *durUs = (int64_t)(sec * 1e6);
  }
  return 0;
}

int dsd_read(float* out, int frames) {
  if (g_fd < 0 || !out || frames <= 0) return 0;
  int produced = 0;
  while (produced < frames) {
    // ① 先吃缓冲
    if (g_outPos < g_out.size()) {
      const size_t avail = (g_out.size() - g_outPos) / (size_t)g_ch;
      size_t take = (size_t)(frames - produced);
      if (take > avail) take = avail;
      std::memcpy(out + (size_t)produced * (size_t)g_ch, g_out.data() + g_outPos, take * (size_t)g_ch * sizeof(float));
      g_outPos += take * (size_t)g_ch;
      produced += (int)take;
      if (g_outPos >= g_out.size()) { g_out.clear(); g_outPos = 0; }
      continue;
    }
    if (g_eos) break;

    // ② 读一块原始字节 → 展开 ±1 → 抽取 → 中间率
    std::vector<uint8_t> raw;
    const int bytesPerCh = 2048;
    const int got = read_chunk(raw, bytesPerCh);
    if (got <= 0) { g_eos = 1; break; }
    const int bits = got * 8;
    g_mid.clear();
    g_mid.reserve((size_t)(bits / g_D + 2) * (size_t)g_ch);
    for (int c = 0; c < g_ch; ++c) {
      const uint8_t* p = raw.data() + (size_t)c * (size_t)got;
      // 可用范围：[g_base - kTail, g_base + bits - 1]
      // 上一块已经产到 floor((base-1)/D)；这里从下一个开始，避免块边界重复出一个样本
      const int64_t first = (g_base == 0) ? 0 : (int64_t)((g_base - 1) / (uint64_t)g_D) + 1;
      const int64_t last = (int64_t)((g_base + (uint64_t)bits - 1) / (uint64_t)g_D);
      for (int64_t m = first; m <= last; ++m) {
        float acc = 0.0f;
        for (int k = 0; k < kTaps; ++k) {
          const int64_t gi = m * (int64_t)g_D - (int64_t)(kTaps - 1) + (int64_t)k;
          float xv = 0.0f;
          if (gi >= (int64_t)g_base) {
            const int64_t rel = gi - (int64_t)g_base;
            if (rel < bits) {
              const uint8_t by = p[rel >> 3];
              const int bit = (g_kind == 1) ? (int)(rel & 7) : (int)(7 - (rel & 7));
              xv = ((by >> bit) & 1) ? 1.0f : -1.0f;
            }
          } else {
            const int64_t rel = gi - ((int64_t)g_base - (int64_t)g_tailCount);
            if (rel >= 0 && rel < g_tailCount) xv = g_tail[c][rel];
          }
          acc += g_h[k] * xv;
        }
        g_mid.push_back(acc);
      }
    }
    // 交错：上面是"声道外层、样本内层"，这里转成逐帧交错
    if (g_ch == 2) {
      const size_t n = g_mid.size() / 2;
      std::vector<float> iv(n * 2);
      for (size_t i = 0; i < n; ++i) { iv[i * 2] = g_mid[i]; iv[i * 2 + 1] = g_mid[n + i]; }
      g_mid.swap(iv);
    }

    // ③ 更新历史 + 全局游标
    for (int c = 0; c < g_ch; ++c) {
      const int keep = bits < kTail ? bits : kTail;
      const int64_t fromGlobal = (int64_t)g_base + (int64_t)bits - keep;
      for (int k = 0; k < keep; ++k) {
        const int64_t rel = fromGlobal + k - (int64_t)g_base;
        const uint8_t by = raw[(size_t)c * (size_t)got + (size_t)(rel >> 3)];
        const int bit = (g_kind == 1) ? (int)(rel & 7) : (int)(7 - (rel & 7));
        g_tail[c][k] = ((by >> bit) & 1) ? 1.0f : -1.0f;
      }
      g_tailCount = keep;
    }
    g_base += (uint64_t)bits;

    // ④ 中间率 → 目标率
    if (!g_mid.empty()) {
      const int midFrames = (int)(g_mid.size() / (size_t)g_ch);
      const int cap = resample_outFramesHint(midFrames) + 8;
      std::vector<float> tmp((size_t)cap * (size_t)g_ch);
      const int n = resample_process(g_mid.data(), midFrames, tmp.data(), cap);
      if (n > 0) {
        g_out.assign(tmp.begin(), tmp.begin() + (size_t)n * (size_t)g_ch);
        g_outPos = 0;
      }
    }
  }
  return produced;
}

int dsd_seekMs(int64_t ms) {
  if (g_fd < 0 || ms < 0) return -1;
  const uint64_t bit = (uint64_t)((double)ms * (double)g_bitRate / 1000.0);
  uint64_t byteIdx = bit / 8ull;
  if (g_kind == 1) byteIdx = (byteIdx / g_blockSize) * g_blockSize;   // DSF 按块对齐
  const uint64_t perCh = g_dataSize / (uint64_t)g_ch;
  if (byteIdx > perCh) byteIdx = perCh;
  g_posBytes = byteIdx;
  g_base = byteIdx * 8ull;
  g_tailCount = 0;
  std::memset(g_tail, 0, sizeof(g_tail));
  g_out.clear();
  g_outPos = 0;
  g_eos = 0;
  resample_reset();
  return 0;
}

int dsd_eos(void) { return g_eos ? 1 : 0; }

void dsd_close(void) {
  if (g_fd >= 0) { ::close(g_fd); g_fd = -1; }
  g_kind = 0;
  g_ch = 0;
  g_bitRate = 0;
  g_dataOff = g_dataSize = g_totalBits = 0;
  g_posBytes = 0;
  g_base = 0;
  g_tailCount = 0;
  g_out.clear();
  g_outPos = 0;
  g_eos = 0;
}

const char* dsd_error(void) { return g_err; }

}  // extern "C"
