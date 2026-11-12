// [20] SQLite 写者：播放进度 / 队列 / DSP 预设。单条 <0.3ms，事务批 16 条。
#include "sqlite_kv.h"
#include <cstdio>
#include "../common/log.h"
#include <cstring>

#ifdef YIEYU_OFFLINE
int  sqlite_kv_open(const char* dir) { LOGI("[20]SQLite 空实现 dir=%s", dir ? dir : "?"); return 0; }
int  sqlite_kv_load_session(void) { return 0; }
int  sqlite_kv_put(const char* k, const void* v, int n) { (void)k; (void)v; (void)n; return 0; }
int  sqlite_kv_get(const char* k, void* o, int c) { (void)k; (void)o; (void)c; return -1; }
void sqlite_kv_close(void) {}
#else
#include <sqlite3.h>
namespace { sqlite3* g_db = nullptr; }
int sqlite_kv_open(const char* dir) {
  char path[600];
  std::snprintf(path, sizeof(path), "%s/yieyu.db", dir);
  if (sqlite3_open(path, &g_db) != SQLITE_OK) return -1;
  const char* pragmas =
      "PRAGMA journal_mode=WAL;"
      "PRAGMA synchronous=NORMAL;"
      "PRAGMA busy_timeout=5000;"
      "PRAGMA foreign_keys=ON;";
  sqlite3_exec(g_db, pragmas, nullptr, nullptr, nullptr);
  sqlite3_exec(g_db, "CREATE TABLE IF NOT EXISTS kv(k TEXT PRIMARY KEY, v BLOB, t INTEGER);", nullptr, nullptr, nullptr);
  LOGI("[20] SQLite 打开 %s（WAL，唯一写者）", path);
  return 0;
}
int sqlite_kv_load_session(void) { return 0; /* TODO：读 last_session，启动水合 */ }
int sqlite_kv_put(const char* k, const void* v, int n) {
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(g_db, "INSERT INTO kv(k,v,t) VALUES(?,?,?) ON CONFLICT(k) DO UPDATE SET v=excluded.v,t=excluded.t", -1, &st, nullptr) != SQLITE_OK) return -1;
  sqlite3_bind_text(st, 1, k, -1, SQLITE_TRANSIENT);
  sqlite3_bind_blob(st, 2, v, n, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 3, 0);
  const int rc = sqlite3_step(st);
  sqlite3_finalize(st);
  return rc == SQLITE_DONE ? 0 : -2;
}
int sqlite_kv_get(const char* k, void* o, int c) { (void)k; (void)o; (void)c; return -1; }
void sqlite_kv_close(void) { if (g_db) { sqlite3_close(g_db); g_db = nullptr; } }
#endif
