// [16] DSD（DSF/DFF）→ PCM。1bit 流 → FIR 抽取（÷8）→ [14] 多相重采样 → 目标采样率。
//
// 为什么自己写：Eigen 不引（零第三方二进制，见 third_party/README.md），
// 而且 DSD 解码本身不需要矩阵库 —— 需要的是"抗混叠 FIR + 抽取"，两样都在这里。
//
// 线程：只允许解码线程调用（会阻塞读盘）。
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 1 = 这个文件是 DSF/DFF；0 = 不是；<0 = 读不了
int dsd_probe(const char* path);

// 打开并解到 targetRate（48000/88200/96000/176400/192000）。
// outRate 返回实际目标采样率，outCh 返回声道数（1 或 2），durUs 返回时长（未知 0）。
int dsd_open(const char* path, int targetRate, int* outRate, int* outCh, int64_t* durUs);

// 读最多 frames 个交错帧到 out，返回帧数；0 = 暂时没有（需继续喂）或结束。
int dsd_read(float* out, int frames);

int dsd_seekMs(int64_t ms);      // 0=成功
int dsd_eos(void);               // 1=真的读完了
void dsd_close(void);
const char* dsd_error(void);

#ifdef __cplusplus
}
#endif
