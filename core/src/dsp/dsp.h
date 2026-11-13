// [17] DSP 音效链的统一入口。三个模块各自只干一件事（铁律 1）：
//   eq31_*        31 段 1/3 倍频程 EQ（ISO 频率表）
//   tube_vinyl_*  胆机软削波 + 黑胶味精 + 16k 陡降 LPF
//   hrtf8d_*      8D 空间化（吃 [23] 的姿态，自动旋转/头追共用同一入口）
//
// ★ 谁调：**解码线程**（player.cpp 解码完、进 PCM 环之前）。
//   音频线程（AAudio 回调）绝不许调 —— 那里只允许读环与算增益（铁律 2）。
// ★ 默认**不启用**：链上任何一级都会改变音色，没界面之前一律旁路，
//   由 JSB 的 Yieyu_SetDsp(1) 显式打开。
#pragma once
namespace yieyu {

void eq31_init(int sampleRate);
void eq31_set_gain(int band, float db);          // band 0..30，db 单位 dB
void eq31_process(float* interleaved, int frames);
int  eq31_points(float* outDb);                  // 把 31 个 dB 点写出去（喂 [2] Rive）

void tube_vinyl_init(int sampleRate);
void tube_vinyl_process(float* interleaved, int frames);
void tube_vinyl_set(bool tube, bool vinyl, bool lpf, float drive);

void hrtf8d_init(int sampleRate);
void hrtf8d_set_pose(float yawDeg, float pitchDeg, int source);   // source: 0=头追 1=自动旋转
void hrtf8d_process(float* interleaved, int frames, int sampleRate);

}  // namespace yieyu
