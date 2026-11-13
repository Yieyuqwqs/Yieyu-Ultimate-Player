// [20] 本地持久化：**唯一写者**。自研追加日志 + CRC + 断电截断 + 阈值压实。
//
// 为什么不用 SQLite：手机侧没有 SQLite 头文件与静态库，引一份 amalgamation 就破了
// 「零第三方二进制」的底线。本工程只要 KV（会话 / 播放进度 / DSP 预设），不需要 SQL。
//   ​记录 = [magic "YKV1" u32][klen u16][vlen u32][crc u32][key][val]；vlen=0 是删除墓碑。
//   打开时逐条校验，遇到半条（断电写坏）就截断到最后一条完整记录。
//
// 铁律（ARCH_24 §0 铁律 4）：写者只有本模块；[19]（Rust）以只读方式读同一个文件。
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

int  kv_open(const char* dir);
int  kv_put(const char* key, const void* val, int len);
int  kv_get(const char* key, void* out, int cap);   // 返回字节数；<0 = 没有这个键
int  kv_del(const char* key);
int  kv_count(void);
int  kv_flush(void);
void kv_close(void);
const char* kv_path(void);
const char* kv_last_error(void);

#ifdef __cplusplus
}
#endif
