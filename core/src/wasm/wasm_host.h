// [8] 自研 wasm 宿主：跑下发的音效插件（热更新，不改 APK）。
//
// 为什么不是 WasmEdge：本工程零第三方二进制（third_party/README.md）。
// 这里是一个只覆盖"音效插件"所需指令子集的 MVP 解释器，guest 由 tools/mkwasm.mjs 生成，
// 两边共用同一 ABI：
//   memory ; abi()->i32 ; init(i32 sampleRate, f32 lfoHz)->i32 ; process(i32 ptr,i32 frames)->i32
//
// 能力边界（刻意收紧，不是漏写）：无 WASI、无 import、无表/全局、内存上限 1 页、
// 指令数上限（防止死循环把音频线程拖死）。插件只在解码线程被调用。
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

int  wasm_host_init(const char* pluginDir);      // 扫描目录下 *.wasm，加载第一个可用的
int  wasm_host_reload(const char* pluginDir);    // = init（导入新插件后调用）
int  wasm_plugin_load(const char* path);         // 0=成功
int  wasm_plugin_process(float* interleaved, int frames);  // 0=成功；<0=没插件/执行错
int  wasm_plugin_ready(void);                    // 1=有插件且 init 过
int  wasm_plugin_count(void);                    // 扫描到几个 .wasm
const char* wasm_host_error(void);
void wasm_host_shutdown(void);

#ifdef __cplusplus
}
#endif
