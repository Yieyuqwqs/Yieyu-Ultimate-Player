// [18] 离线人声消除（KTV 伴奏）。只走"离线/非实时"通道：整曲预处理，绝不进 AAudio 回调。
//
// 为什么不是 ONNX Runtime：本工程零第三方二进制（third_party/README.md）。
// 这里用的是成熟的中置抑制算法（mid/side 功率掩码 + 频率权重），
// 核系数（分析窗 + 权重曲线）由 Python 在**构建期**生成（core/src/ai/vocal_kernel.h）。
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// srcWav → dstWav（PCM16，立体声）。onProgress 每帧回调一次（0..1），可为 NULL。
// 返回 0=成功；<0=错误（见 yieyu_vocals_error）。
int yieyu_vocals_render(const char* srcWav, const char* dstWav, void (*onProgress)(float));
const char* yieyu_vocals_error(void);

#ifdef __cplusplus
}
#endif
