// [13] 解封装+解码的**平台后端**（NDK 自带 AMediaExtractor / AMediaCodec）。
//
// 为什么要它：仓库里拿不到 FFmpeg 预编译库时（CI 的 YIEYU_OFFLINE_ASSETS=ON），
// FFmpeg 后端整块是空实现，结果就是"能搜不能放"。平台后端零第三方依赖、
// 零额外 .so，直接吃系统解码器，mp3/flac/m4a/aac 都能出 PCM。
//
// 职责边界（铁律 1）：只做"解封装 + 解码 → float32 立体声交错"，
// 重采样归 [14]，DSP 归 [17]，出声归 [15]。绝不在本文件里开线程、做效果、写文件。
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 打开一个本地路径或 http(s) URL。**会阻塞**（网络拉流），只允许解码线程调用。
// rate/channels 返回源参数；durUs 返回时长（未知时 0）。0=成功，<0=错误码。
int yieyu_pdec_open(const char* uri, int32_t* rate, int32_t* channels, int64_t* durUs);

// 读最多 frames 个立体声帧到 out（交错 float）。返回实际帧数；0=暂时没数据或已到末尾。
int yieyu_pdec_read(float* out, int frames);

// 跳转（微秒）。0=成功。
int yieyu_pdec_seek(int64_t posUs);

// 1=已经读到流末尾（配合 read()==0 判断真播完）
int yieyu_pdec_eos(void);

void yieyu_pdec_close(void);

// 最近一次 open 失败的原因（给开发者面板；正常界面不展示）。
// 例：路径打开失败 st=-10000，open 也失败 errno=13 / setDataSourceFd 失败 st=-10000（0 字节）
const char* yieyu_pdec_error(void);

#ifdef __cplusplus
}
#endif
