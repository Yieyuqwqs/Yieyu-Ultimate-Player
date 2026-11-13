// [14] 重采样器的对外契约（音质链路上只有 player.cpp 会调）。
#pragma once

namespace yieyu {
int  resample_init(int inRate, int outRate, int channels);   // 0=成功
void resample_reset(void);
int  resample_process(const float* in, int frames, float* out, int outCap);  // 返回帧数
int  resample_outRate(void);
int  resample_taps(void);
int  resample_ratio(int* up, int* down);                     // out/in = up/down
int  resample_outFramesHint(int frames);
}  // namespace yieyu
