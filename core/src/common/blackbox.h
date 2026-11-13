// 【黑匣子】物理日志：只在 ERROR / FATAL 落盘，异步写，永不阻塞 UI / 音频线程。
//
// 设计铁律（见 docs/BLUEPRINT.md §日志规范）：
//   · 成功的流程一行都不打（避免 I/O 与内存增长）
//   · 只有 ERROR/FATAL 才写，写之前先把"时间/trace/模块/入参/堆栈"全拍下来
//   · 5MB 轮转，最多 3 个历史文件，最旧的自动删
//   · 首选 /storage/emulated/0/yieyu/；没有权限就降级到 App 私有目录，绝不因此崩
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { YIEYU_LOG_ERROR = 1, YIEYU_LOG_FATAL = 2 };

// dir 由 app_paths 决定（yp_home()），黑匣子**不再自己探测**公共/私有。
// 0=成功；-1=参数错；-2=目录建不出来。
int  bb_init(const char* dir);
const char* bb_dir(void);
int  bb_is_public(void);          // 1 = 日志真的在 /storage/emulated/0/yieyu 下

// 只记 ERROR/FATAL。fmt 里以 "[13] ..." 开头会自动解析成模块号。
void bb_write(int level, const char* fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

// trace_id：每条 UI 命令生成一个，贯穿整条链路。音频线程不用（也不许调）。
void        bb_new_trace(void);
const char* bb_trace(void);
void        bb_set_trace(const char* id);

// 导出：把现有日志拷到 <publicDir>/exported/ 方便文件管理器直接拿。返回最终目录。
int  bb_export(char* out, size_t cap);
void bb_flush(void);              // 阻塞到队列写完（只在导出/退出时调）
void bb_shutdown(void);
uint32_t bb_dropped(void);        // 队列满时丢了几条（自身也要能查）

#ifdef __cplusplus
}
#endif
