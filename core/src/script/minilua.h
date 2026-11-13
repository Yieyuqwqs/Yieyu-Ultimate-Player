// [7] 自研 Lua 子集解释器（零依赖：不引 LuaJIT、不引任何第三方）
//
// 支持（够跑音源脚本）：
//   local / 赋值 / function 定义（含 local function）/ if-elseif-else / while / 数值 for /
//   泛型 for-in（ipairs/pairs）/ return / break / do / 表构造 / a.b a[b] / f(...) f{...} /
//   a:b(...) / 算术 + - * / % / 连接 .. / 比较 == ~= < <= > >= / 逻辑 and or not / # / 一元 -
// 不支持（明确报错，不假装能跑）：元表、协程、goto、多返回值（除 pcall）、上值写入
//
// 宿主库：http_get / http_post / request / fetch / json.encode / json.decode / log /
//         tostring / tonumber / type / pcall / ipairs / pairs / table.insert / table.concat /
//         math.floor / math.min / math.max / os.time
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// 每个 .lua 脚本一个 VM 状态（互相隔离，全局变量不串）
void* ml_new(void);
void  ml_free(void* vm);

// 加载脚本源码（失败返回非 0，原因进 ml_error）
int ml_load(void* vm, const char* name, const char* src, size_t len);

// 调用脚本里的全局函数；argv 为 UTF-8 字符串数组（NULL 表示该参数是 nil）。
// out/outCap 收返回值（字符串化）；返回 0=成功。
int ml_call(void* vm, const char* fn, const char* const* argv, int argc, char* out, int outCap);

// 最近一次错误（含 Lua 报错位置）
const char* ml_error(void* vm);

// 自检：跑一段内置脚本（校验词法/语法/执行/表/循环/函数调用都通）
int ml_selftest(void);

#ifdef __cplusplus
}
#endif
