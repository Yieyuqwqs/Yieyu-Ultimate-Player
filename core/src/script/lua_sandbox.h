// 【模块 7】LuaJIT 音源脚本沙箱 —— 热更入口就在这里。
//
// 设计目标：改搜歌规则 = 把 .lua 丢进目录 + 点一下"重新加载"，不打包、不重启。
// 安全边界：白名单 API、指令计数、黑名单静态检查（与 tools/collect-lua.mjs 同源）。
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int  lua_sandbox_init(const char* scriptsDir);
void lua_sandbox_set_limit(uint64_t instr);

// 执行单个脚本；out 收结果（JSON 字符串），cap 是容量
int  lua_sandbox_run(const char* scriptName, const char* input, char* out, int cap);

// 热更：扫描 dir 下所有 .lua，逐个做黑名单校验并登记。
// loaded = 通过的脚本数，rejected = 被拒的脚本数（被拒的**不会**加载）
int  lua_sandbox_reload(const char* dir, int* loaded, int* rejected);

// 再叠加一个脚本目录（同名以先扫到的为准）。用于「公共目录不可写 → 私有目录兜底」。
int  lua_sandbox_add_dir(const char* dir, int* loaded, int* rejected);

void lua_sandbox_shutdown(void);

#ifdef __cplusplus
}
#endif
