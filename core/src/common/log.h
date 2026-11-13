// 日志门面。规范见 docs/BLUEPRINT.md §日志铁律：
//   · 成功流程一律静默（LOGI / LOGW 都是 no-op）——内存和 I/O 都是成本
//   · 只有 ERROR / FATAL 才落到黑匣子（异步线程，永不阻塞）
#pragma once
#include "blackbox.h"

#define LOGI(...) ((void)0)                                   // 成功 → 一个字都不打
#define LOGW(...) ((void)0)                                   // 可恢复 → 也不打
#define LOGE(...) bb_write(YIEYU_LOG_ERROR, __VA_ARGS__)       // 错误 → 黑匣子
#define LOGF(...) bb_write(YIEYU_LOG_FATAL, __VA_ARGS__)       // 致命 → 黑匣子 + 堆栈

// 标记：此函数可能在实时线程被调用（实时路径里禁止任何日志调用）
#define YIEYU_RT_UNSAFE
