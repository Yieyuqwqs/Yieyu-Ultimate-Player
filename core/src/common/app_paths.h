// 【路径唯一真相】App 里所有「往哪写」的决定只在这里做一次。
//
// 为什么必须有它（血泪史）：
//   黑匣子自己探测公共目录、jsb 又自己拼一套 scripts 路径、建目录用裸 mkdir（只能建一层）……
//   三处各说各话，结果就是「装完重装后导入脚本必失败」这种问题查了三轮才定位。
//
// 规矩（谁违反谁负责）：
//   1. 只有 yp_init() 决定 home 落在公共目录还是私有目录，其它模块**一律不许再探测**；
//   2. 只有 yp_ensure() 允许 mkdir（递归、幂等）；
//   3. 需要路径就调 yp_home() / yp_scripts() / yp_data()，不许自己 snprintf 拼存储根。
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// 必须在任何模块之前调且只调一次（Engine::Boot 第一句）。
// dataDir = App 私有目录（Dart 的 getApplicationSupportDirectory），永远可写。
// 返回 1 = home 在公共目录；0 = home 在私有目录。
int yp_init(const char* dataDir);

const char* yp_home(void);      // 日志/导出/脚本的根（init 后一定存在）
const char* yp_scripts(void);   // <home>/scripts（init 后一定存在）
const char* yp_data(void);      // App 私有目录（SQLite 等只能放这里）
int  yp_is_public(void);        // 1 = home 真的落在 /storage/emulated/0/yieyu

// 递归创建（本工程**唯一**的 mkdir 实现）。0=成功/已存在，-1=失败。
int yp_ensure(const char* dir);

// 真的写一个探针文件来判断目录可写（access(W_OK) 在 Android 上会骗人）
int yp_writable(const char* dir);

#ifdef __cplusplus
}
#endif
