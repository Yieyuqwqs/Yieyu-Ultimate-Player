// [16] Eigen：DSD 的 1bit 流 → PCM 的多相抽取 + 滤波矩阵运算。
// DSD64 = 2822400 Hz / 64 → 44100；DSD128/256 同理。矩阵是常量，编译期生成。
#include "../common/log.h"

#ifdef YIEYU_OFFLINE
namespace yieyu {
int  dsd_init(int dsdMultiple) { (void)dsdMultiple; LOGI("[16]Eigen 空实现 dsd x%d", dsdMultiple); return 0; }
int  dsd_to_pcm(const uint8_t* dsd, int bytes, float* pcm) { (void)dsd; (void)bytes; (void)pcm; return 0; }
}
#else
#include <Eigen/Dense>
namespace yieyu {
namespace {
  Eigen::MatrixXf g_decim;   // 128 × 64 抗混叠 FIR（离线设计，编译期固化）
  int g_multiple = 64;
}
int dsd_init(int dsdMultiple) {
  g_multiple = dsdMultiple;
  // TODO(P3)：用窗函数法离线生成，导出为 C 数组（不许运行期反三角函数开表）
  g_decim = Eigen::MatrixXf::Zero(128, 64);
  LOGI("[16] Eigen DSD x%d 矩阵 %dx%d", dsdMultiple, (int)g_decim.rows(), (int)g_decim.cols());
  return 0;
}
int dsd_to_pcm(const uint8_t* dsd, int bytes, float* pcm) {
  const int samples = bytes * 8;           // 1 bit = 1 sample
  Eigen::Map<const Eigen::VectorXf> bits(nullptr, 0);
  (void)samples; (void)bits; (void)pcm;
  return 0;                                // TODO(P3)：bit→±1 → 矩阵抽取 → LPF
}
}
#endif
