// [8] WasmEdge 宿主：跑下发的音效插件（热更新，不改 APK）。
// 能力收紧：无 WASI 文件/网络、内存上限 32MB、栈 1MB、禁 JIT（Android W^X 会拦）。
#include "../common/log.h"
#include <cstdint>

#ifdef YIEYU_OFFLINE
namespace yieyu {
int  wasm_host_init(const char* pluginDir) { LOGI("[8]WasmEdge 空实现 dir=%s", pluginDir ? pluginDir : "?"); return 0; }
int  wasm_plugin_load(const char* path) { (void)path; return -1; }
int  wasm_plugin_process(const char* name, float* io, int frames) { (void)name; (void)io; (void)frames; return 0; }
void wasm_host_shutdown() {}
}
#else
namespace yieyu {
namespace { /* TODO(P3)：WasmEdge_VMRegisterModuleFromFile + host 函数白名单 */ }
int  wasm_host_init(const char* pluginDir) { (void)pluginDir; return 0; }
int  wasm_plugin_load(const char* path) { (void)path; return -1; }
int  wasm_plugin_process(const char* n, float* io, int frames) { (void)n; (void)io; (void)frames; return 0; }
void wasm_host_shutdown() {}
}
#endif
