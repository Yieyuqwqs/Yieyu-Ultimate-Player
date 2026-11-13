//! 【模块 19】本地库只读半边（零依赖：std::fs + 内存索引）。
//!
//! 边界铁律（docs/ARCH_24.md 铁律 4）：写者只有模块 20（core/src/store/kv_store.cpp）。
//! 这里只 mmap/read 同一个文件，一次都不写。为什么坚持异步：读盘最坏是慢速 eMMC 上的
//! 所以读盘只在 Boot 里做一次：解出索引后，后续查询全在内存里完成（微秒级）。
//!
//! 文件格式与模块 20 逐字节一致：
//!   [magic "YKV1" u32][klen u16][vlen u32][crc u32][key][val]，vlen=0 是删除墓碑。

use std::collections::HashMap;
use std::ffi::CStr;
use std::os::raw::{c_char, c_int};
use std::sync::{Mutex, OnceLock};

const REC_MAGIC: u32 = 0x3156_4B59; // "YKV1"
const HDR: usize = 4 + 2 + 4 + 4;

struct Store {
    map: HashMap<String, Vec<u8>>,
    bytes: u64,
}

static STORE: OnceLock<Mutex<Option<Store>>> = OnceLock::new();

fn slot() -> &'static Mutex<Option<Store>> {
    STORE.get_or_init(|| Mutex::new(None))
}

fn crc32(data: &[u8]) -> u32 {
    let mut c: u32 = 0xFFFF_FFFF;
    for &b in data {
        c ^= b as u32;
        for _ in 0..8 {
            c = if c & 1 != 0 { 0xEDB8_8320 ^ (c >> 1) } else { c >> 1 };
        }
    }
    !c
}

/// 逐条校验；坏记录（断电残留）直接停，前面解出来的照常可用。
fn parse(buf: &[u8]) -> Store {
    let mut map: HashMap<String, Vec<u8>> = HashMap::new();
    let mut off = 0usize;
    while off + HDR <= buf.len() {
        let magic = u32::from_le_bytes(buf[off..off + 4].try_into().unwrap());
        let klen = u16::from_le_bytes(buf[off + 4..off + 6].try_into().unwrap()) as usize;
        let vlen = u32::from_le_bytes(buf[off + 6..off + 10].try_into().unwrap()) as usize;
        let crc = u32::from_le_bytes(buf[off + 10..off + 14].try_into().unwrap());
        if magic != REC_MAGIC || klen == 0 || klen > 512 {
            break;
        }
        let end = off + HDR + klen + vlen;
        if end > buf.len() {
            break;
        }
        let key = &buf[off + HDR..off + HDR + klen];
        let val = &buf[off + HDR + klen..end];
        let mut c = crc32(key);
        if !val.is_empty() {
            // 与模块 20 一致：crc 是 key 之后继续 update val 的累计值
            c = crc32_chain(c, val);
        }
        if c != crc {
            break;
        }
        match std::str::from_utf8(key) {
            Ok(k) => {
                if val.is_empty() {
                    map.remove(k);
                } else {
                    map.insert(k.to_owned(), val.to_vec());
                }
            }
            Err(_) => break,
        }
        off = end;
    }
    Store { map, bytes: off as u64 }
}

/// crc32_update(seed=前一段的结果, next) —— 与 C 侧 yieyu::crc32_update 一致
fn crc32_chain(seed: u32, data: &[u8]) -> u32 {
    let mut c: u32 = seed ^ 0xFFFF_FFFF;
    for &b in data {
        c ^= b as u32;
        for _ in 0..8 {
            c = if c & 1 != 0 { 0xEDB8_8320 ^ (c >> 1) } else { c >> 1 };
        }
    }
    c ^ 0xFFFF_FFFF
}

fn cstr(p: *const c_char) -> Option<String> {
    if p.is_null() {
        return None;
    }
    unsafe { CStr::from_ptr(p) }.to_str().ok().map(|s| s.to_owned())
}

#[no_mangle]
pub extern "C" fn yieyu_store_open(path: *const c_char) -> c_int {
    let Some(path) = cstr(path) else { return -1 };
    let data = match std::fs::read(&path) {
        Ok(d) => d,
        Err(_) => return -2,
    };
    let s = parse(&data);
    match slot().lock() {
        Ok(mut g) => {
            *g = Some(s);
            0
        }
        Err(_) => -3,
    }
}

#[no_mangle]
pub extern "C" fn yieyu_store_count() -> c_int {
    match slot().lock() {
        Ok(g) => g.as_ref().map_or(-1, |s| s.map.len() as c_int),
        Err(_) => -1,
    }
}

#[no_mangle]
pub extern "C" fn yieyu_store_bytes() -> i64 {
    match slot().lock() {
        Ok(g) => g.as_ref().map_or(-1, |s| s.bytes as i64),
        Err(_) => -1,
    }
}

/// 只读取键：返回原始长度（可能 > cap，此时只拷 cap 字节）；<0 表示没有这个键。
#[no_mangle]
pub extern "C" fn yieyu_store_get(key: *const c_char, out: *mut u8, cap: c_int) -> c_int {
    let Some(key) = cstr(key) else { return -1 };
    if out.is_null() || cap <= 0 {
        return -1;
    }
    let Ok(g) = slot().lock() else { return -1 };
    let Some(s) = g.as_ref() else { return -2 };
    let Some(v) = s.map.get(&key) else { return -3 };
    let n = v.len().min(cap as usize);
    unsafe { std::ptr::copy_nonoverlapping(v.as_ptr(), out, n) };
    v.len() as c_int
}

#[no_mangle]
pub extern "C" fn yieyu_store_close() {
    if let Ok(mut g) = slot().lock() {
        *g = None;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn rec(key: &str, val: &[u8]) -> Vec<u8> {
        let mut out = Vec::new();
        out.extend_from_slice(&REC_MAGIC.to_le_bytes());
        out.extend_from_slice(&(key.len() as u16).to_le_bytes());
        out.extend_from_slice(&(val.len() as u32).to_le_bytes());
        let c = if val.is_empty() {
            crc32(key.as_bytes())
        } else {
            crc32_chain(crc32(key.as_bytes()), val)
        };
        out.extend_from_slice(&c.to_le_bytes());
        out.extend_from_slice(key.as_bytes());
        out.extend_from_slice(val);
        out
    }

    #[test]
    fn roundtrip_and_tombstone() {
        let mut b = rec("a", b"1");
        b.extend(rec("b", b"22"));
        b.extend(rec("a", b"333"));
        b.extend(rec("b", b"")); // 删除
        b.extend_from_slice(&[0u8; 5]); // 断电残留：半条
        let s = parse(&b);
        assert_eq!(s.map.len(), 1);
        assert_eq!(s.map.get("a").unwrap(), b"333");
    }
}
