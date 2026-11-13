// [11] 防篡改：读出**磁盘上的自己**算 SHA-256，再和构建期写进 APK 资产里的期望值比对。
// 比对本身走 [12]（ARM64 NEON 常数时间），因为摘要比较若提前 return，理论上能靠计时猜前缀。
//
// 为什么不用 Obfuscator-LLVM：那是自建 LLVM 分支，NDK r27 起 GCC 前端已废弃，
// 套不上。本工程用"符号收敛 + ICF + 链接后 strip/重命名 + 运行时完整性自校验"组合替代，
// 目标一样：拿到 so 改一个字节就自曝。
#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// 自身 .so 的绝对路径（dl_iterate_phdr 查出来）；查不到返回空串。
const char* yieyu_self_path(void);

// 计算自身 sha256 → 64 字节小写 hex + '\0'。0=成功。
int yieyu_self_digest(char outHex[65]);

// 与期望值比对：1=一致 0=被改过 -1=取不到自身（宿主测试/静态链接）。
int yieyu_integrity_check(const char* expectHex);

#ifdef __cplusplus
}
#endif
