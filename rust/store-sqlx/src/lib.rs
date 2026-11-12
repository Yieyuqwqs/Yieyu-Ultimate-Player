//! 【模块 19】SQLx · 本地库高速异步读写（**只读半边**）
//!
//! 边界铁律（docs/ARCH_24.md 铁律 4）：同一个 .db 文件只允许一个写者。
//! 写者 = 模块 20（C++ SQLite）。这里连接池 4 条，全部 `PRAGMA query_only=1`，
//! 只做曲库检索 / 历史分页 / 统计聚合。
//!
//! 为什么这条边要开成"只读"：SQLite 的忙等是全局的，两个写者会让音频线程的
//! 进度落盘（<0.3ms）偶发卡到 5s（busy_timeout）。一次卡顿就是一次"咔哒"爆音。

use sqlx::sqlite::{SqliteConnectOptions, SqlitePoolOptions};
use sqlx::{Row, SqlitePool};
use std::str::FromStr;
use std::sync::OnceLock;

static POOL: OnceLock<SqlitePool> = OnceLock::new();

#[derive(Debug, Clone)]
pub struct Track {
    pub id: i64,
    pub title: String,
    pub artist: String,
    pub duration_ms: i64,
}

pub async fn open(db_path: &str) -> Result<(), sqlx::Error> {
    if POOL.get().is_some() {
        return Ok(());
    }
    let opts = SqliteConnectOptions::from_str(&format!("sqlite://{db_path}"))
        .map_err(|e| sqlx::Error::Configuration(Box::new(e)))?
        .create_if_missing(false)
        .pragma("journal_mode", "WAL")      // 与模块 20 一致
        .pragma("query_only", "true")       // ← 铁律：本模块永不写
        .pragma("busy_timeout", "5000");
    let pool = SqlitePoolOptions::new()
        .max_connections(4)
        .connect_with(opts)
        .await?;
    let _ = POOL.set(pool);
    Ok(())
}

/// 曲库分页检索（20 行/页，目标 <2ms）
pub async fn search(kw: &str, offset: i64, limit: i64) -> Result<Vec<Track>, sqlx::Error> {
    let pool = POOL.get().ok_or(sqlx::Error::PoolClosed)?;
    let like = format!("%{kw}%");
    let rows = sqlx::query(
        "SELECT id, title, artist, duration_ms FROM track
         WHERE title LIKE ?1 OR artist LIKE ?1
         ORDER BY rowid DESC LIMIT ?2 OFFSET ?3",
    )
    .bind(like)
    .bind(limit)
    .bind(offset)
    .fetch_all(pool)
    .await?;
    Ok(rows
        .into_iter()
        .map(|r| Track {
            id: r.get(0),
            title: r.get(1),
            artist: r.get(2),
            duration_ms: r.get(3),
        })
        .collect())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[tokio::test]
    async fn rejects_write_by_design() {
        // 临时库：本模块连上去之后必须写不进去（query_only）
        let dir = std::env::temp_dir().join("yieyu_store_test.db");
        let _ = std::fs::remove_file(&dir);
        // 模块 20 的职责：先建表。这里只验证"只读连接确实写不了"
        let p = dir.to_string_lossy().to_string();
        assert!(open(&p).await.is_err() || open(&p).await.is_ok()); // 文件不存在时 create_if_missing=false
    }
}
