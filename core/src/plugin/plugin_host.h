// [8] 插件宿主：加载/执行 YPFX 字节码插件（自有格式，见 abi.h）。
//
// 能力边界（刻意收紧）：无文件、无网络、无内存分配、无循环指令；一次调用最多 4096 条指令；
// 状态只有 64 个 f32 槽。插件出错只丢当前处理块并自动关开关，绝不把播放带崩。
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

int  plugin_host_init(const char* pluginDir);     // 扫描目录下 *.ypfx，加载第一个可用的
int  plugin_host_reload(const char* pluginDir);   // 导入新插件后调用
int  plugin_load(const char* path);               // 0=成功
int  plugin_ready(void);                          // 1=有插件且已按采样率初始化
int  plugin_count(void);                          // 扫描到几个 .ypfx
int  plugin_process(float* interleaved, int frames);  // 逐点增益，0=成功
int  plugin_on_rate(int sampleRate);              // 起流时告诉插件采样率（算 step）
const char* plugin_error(void);
void plugin_shutdown(void);

#ifdef __cplusplus
}
#endif
