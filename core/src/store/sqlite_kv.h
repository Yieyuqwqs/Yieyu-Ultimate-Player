// [20] SQLite：唯一写者。WAL + busy_timeout，断电安全。
// 模块 19（Rust SQLx）只读同一文件，query_only=1。
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
int  sqlite_kv_open(const char* dir);
int  sqlite_kv_load_session(void);
int  sqlite_kv_put(const char* key, const void* val, int len);
int  sqlite_kv_get(const char* key, void* out, int cap);
void sqlite_kv_close(void);
#ifdef __cplusplus
}
#endif
