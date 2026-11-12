//! 【模块 10】ring · 密码学级验签
//!
//! 只干一件事：验证授权令牌（卡密）是不是我们签的。
//! **全场唯一持有公钥的地方**。Kotlin / Dart / Lua / Wasm 一律不许有密钥副本。
//!
//! 令牌格式（文本，方便用户复制）：
//!   `YIEYU1.<base64url(payload)>.<base64url(sig)>`
//!   payload 里是 `{"u":"用户名","e":过期时间戳,"d":设备指纹前8字节hex,"f":功能位}`
//!
//! 铁律：验签失败不崩进程，只返回错误码，由 Engine 熔断（停解码 / 置灰 UI）。

use ring::signature::{self, UnparsedPublicKey};

/// Ed25519 公钥（32 字节）。生产值不进仓库 —— 由 CI secret 注入，
/// 本地编译缺省用这个全零占位，任何签名都验不过（安全失败）。
const ED25519_PUBKEY: [u8; 32] = {
    #[cfg(feature = "ci-pubkey")]
    {
        include!("../keys/pubkey.bin")
    }
    #[cfg(not(feature = "ci-pubkey"))]
    {
        [0u8; 32]
    }
};

#[repr(C)]
pub enum LicResult {
    Ok = 0,
    Malformed = -1,
    BadPrefix = -2,
    BadBase64 = -3,
    BadSignature = -4,
    Expired = -5,
    DeviceMismatch = -6,
}

fn b64_decode(s: &[u8]) -> Option<Vec<u8>> {
    // 骨架期手写 base64url 解码（零依赖）；P2 换成 `base64ct` 的常数时间版本
    let mut out = Vec::with_capacity(s.len() * 3 / 4);
    let mut acc: u32 = 0;
    let mut bits = 0u32;
    for &c in s {
        let v = match c {
            b'A'..=b'Z' => c - b'A',
            b'a'..=b'z' => c - b'a' + 26,
            b'0'..=b'9' => c - b'0' + 52,
            b'-' => 62,
            b'_' => 63,
            b'=' => break,
            _ => return None,
        };
        acc = (acc << 6) | v as u32;
        bits += 6;
        if bits >= 8 {
            bits -= 8;
            out.push((acc >> bits) as u8);
        }
    }
    Some(out)
}

/// 纯 Rust 版验签（给测试用，也给 core 通过 FFI 调）
pub fn verify(token: &str, now_unix: u64, device_fp: &[u8]) -> LicResult {
    let mut it = token.split('.');
    let (Some(prefix), Some(payload_b64), Some(sig_b64)) = (it.next(), it.next(), it.next()) else {
        return LicResult::Malformed;
    };
    if prefix != "YIEYU1" || it.next().is_some() {
        return LicResult::BadPrefix;
    }
    let (Some(payload), Some(sig)) = (b64_decode(payload_b64.as_bytes()), b64_decode(sig_b64.as_bytes()))
    else {
        return LicResult::BadBase64;
    };
    if sig.len() != 64 {
        return LicResult::BadSignature;
    }

    // ① 签名：对 "YIEYU1.<payload>" 整段验，防前缀替换
    let mut signed = Vec::with_capacity(7 + payload_b64.len());
    signed.extend_from_slice(b"YIEYU1.");
    signed.extend_from_slice(payload_b64.as_bytes());
    let key = UnparsedPublicKey::new(&signature::ED25519, &ED25519_PUBKEY);
    if key.verify(&signed, &sig).is_err() {
        return LicResult::BadSignature;
    }

    // ② 语义：过期 / 设备绑定（骨架期只做最朴素的字段扫描，P2 上 serde_json）
    let text = String::from_utf8_lossy(&payload);
    if let Some(rest) = text.split("\"e\":").nth(1) {
        if let Ok(exp) = rest.split([',', '}']).next().unwrap_or("0").trim().parse::<u64>() {
            if exp != 0 && now_unix > exp {
                return LicResult::Expired;
            }
        }
    }
    if let Some(rest) = text.split("\"d\":\"").nth(1) {
        let d = rest.split('"').next().unwrap_or("");
        if !device_fp.is_empty() && !d.is_empty() && d != hex(device_fp) {
            return LicResult::DeviceMismatch;
        }
    }
    LicResult::Ok
}

fn hex(b: &[u8]) -> String {
    b.iter().map(|x| format!("{x:02x}")).collect()
}

// ── FFI（core 通过 dlopen 一次，句柄常驻）───────────────────────────
#[no_mangle]
pub extern "C" fn yieyu_license_verify(
    token: *const u8,
    len: usize,
    now_unix: u64,
    device_fp: *const u8,
    fp_len: usize,
) -> i32 {
    if token.is_null() || len == 0 {
        return LicResult::Malformed as i32;
    }
    let t = unsafe { std::slice::from_raw_parts(token, len) };
    let Ok(s) = std::str::from_utf8(t) else { return LicResult::Malformed as i32 };
    let fp = if device_fp.is_null() || fp_len == 0 {
        &[][..]
    } else {
        unsafe { std::slice::from_raw_parts(device_fp, fp_len) }
    };
    verify(s, now_unix, fp) as i32
}

/// 给 Kotlin/Dart 做"我这边有没有公钥"的自检（返回 1 表示已注入真公钥）
#[no_mangle]
pub extern "C" fn yieyu_license_has_pubkey() -> i32 {
    if ED25519_PUBKEY.iter().all(|&b| b == 0) { 0 } else { 1 }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn rejects_garbage() {
        assert!(matches!(verify("hello", 0, &[]), LicResult::Malformed));
        assert!(matches!(verify("YIEYU2.a.b", 0, &[]), LicResult::BadPrefix));
        assert!(matches!(verify("YIEYU1.!!!.???", 0, &[]), LicResult::BadBase64));
    }

    #[test]
    fn no_pubkey_means_fail_closed() {
        // 占位公钥下任何签名都过不去 —— 这是刻意的"安全失败"
        assert_eq!(yieyu_license_has_pubkey(), 0);
    }
}
