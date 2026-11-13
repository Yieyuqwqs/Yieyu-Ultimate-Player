//! 【模块 10】卡密验签 —— **零依赖**（不引 ring，也不引任何 crate）。
//!
//! 为什么把 ring 拔了：为了一个验签从 crates.io 拉一整棵依赖树，是外行的做法。
//! 令牌格式（文本，方便用户复制）：
//!   `YIEYU2.<base64url(payload)>.<base64url(hmac-sha256)>`
//!   payload = `{"u":"用户名","e":过期时间戳,"d":设备指纹前8字节hex,"f":功能位}`
//!   hmac    = HMAC-SHA256(KEY, "YIEYU2." + payload_b64)
//!
//! 诚实的边界：HMAC 是**对称**的（能验也能签）。所以这一层只保证"令牌不是随手编的"，
//! 真正的商业授权需要非对称签名（Ed25519）—— 那要自己写椭圆曲线，不在本轮范围。
//! 密钥在编译期以字节数组内嵌（构建期可替换），任何其它模块持有副本都算安全事故。
//!
//! 铁律：验签失败不崩进程，只返回错误码，由 Engine 熔断（停解码 / 置灰 UI）。

#![allow(clippy::missing_safety_doc)]

/// 构建期内嵌的 HMAC 密钥（32 字节）。真实发布时用 CI secret 替换。
/// 这里放一个固定值：它只用于"自用/演示"，泄露也不影响任何人的资产。
const HMAC_KEY: [u8; 32] = [
    0x59, 0x49, 0x45, 0x59, 0x55, 0x2d, 0x55, 0x4c, 0x54, 0x49, 0x4d, 0x41, 0x54, 0x45, 0x2d, 0x50,
    0x4c, 0x41, 0x59, 0x45, 0x52, 0x2d, 0x48, 0x4d, 0x41, 0x43, 0x2d, 0x4b, 0x45, 0x59, 0x2d, 0x31,
];

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

// ── SHA-256（FIPS 180-4，纯 std 实现）─────────────────────────────
const K: [u32; 64] = [
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
];

fn sha256(data: &[u8]) -> [u8; 32] {
    let mut h: [u32; 8] = [
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
    ];
    let bits = (data.len() as u64) * 8;
    let mut msg = data.to_vec();
    msg.push(0x80);
    while msg.len() % 64 != 56 {
        msg.push(0);
    }
    msg.extend_from_slice(&bits.to_be_bytes());

    for chunk in msg.chunks_exact(64) {
        let mut w = [0u32; 64];
        for i in 0..16 {
            w[i] = u32::from_be_bytes([chunk[i * 4], chunk[i * 4 + 1], chunk[i * 4 + 2], chunk[i * 4 + 3]]);
        }
        for i in 16..64 {
            let s0 = w[i - 15].rotate_right(7) ^ w[i - 15].rotate_right(18) ^ (w[i - 15] >> 3);
            let s1 = w[i - 2].rotate_right(17) ^ w[i - 2].rotate_right(19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16].wrapping_add(s0).wrapping_add(w[i - 7]).wrapping_add(s1);
        }
        let (mut a, mut b, mut c, mut d, mut e, mut f, mut g, mut hh) =
            (h[0], h[1], h[2], h[3], h[4], h[5], h[6], h[7]);
        for i in 0..64 {
            let s1 = e.rotate_right(6) ^ e.rotate_right(11) ^ e.rotate_right(25);
            let ch = (e & f) ^ ((!e) & g);
            let t1 = hh.wrapping_add(s1).wrapping_add(ch).wrapping_add(K[i]).wrapping_add(w[i]);
            let s0 = a.rotate_right(2) ^ a.rotate_right(13) ^ a.rotate_right(22);
            let mj = (a & b) ^ (a & c) ^ (b & c);
            let t2 = s0.wrapping_add(mj);
            hh = g;
            g = f;
            f = e;
            e = d.wrapping_add(t1);
            d = c;
            c = b;
            b = a;
            a = t1.wrapping_add(t2);
        }
        h[0] = h[0].wrapping_add(a);
        h[1] = h[1].wrapping_add(b);
        h[2] = h[2].wrapping_add(c);
        h[3] = h[3].wrapping_add(d);
        h[4] = h[4].wrapping_add(e);
        h[5] = h[5].wrapping_add(f);
        h[6] = h[6].wrapping_add(g);
        h[7] = h[7].wrapping_add(hh);
    }
    let mut out = [0u8; 32];
    for i in 0..8 {
        out[i * 4..i * 4 + 4].copy_from_slice(&h[i].to_be_bytes());
    }
    out
}

/// HMAC-SHA256（RFC 2104）
fn hmac_sha256(key: &[u8], msg: &[u8]) -> [u8; 32] {
    let mut k = [0u8; 64];
    if key.len() > 64 {
        k[..32].copy_from_slice(&sha256(key));
    } else {
        k[..key.len()].copy_from_slice(key);
    }
    let mut inner = Vec::with_capacity(64 + msg.len());
    for b in k.iter() {
        inner.push(b ^ 0x36);
    }
    inner.extend_from_slice(msg);
    let ih = sha256(&inner);
    let mut outer = Vec::with_capacity(96);
    for b in k.iter() {
        outer.push(b ^ 0x5c);
    }
    outer.extend_from_slice(&ih);
    sha256(&outer)
}

fn b64url_decode(s: &[u8]) -> Option<Vec<u8>> {
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

fn b64url_encode(data: &[u8]) -> String {
    const T: &[u8; 64] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    let mut out = String::new();
    for chunk in data.chunks(3) {
        let b0 = chunk[0] as u32;
        let b1 = *chunk.get(1).unwrap_or(&0) as u32;
        let b2 = *chunk.get(2).unwrap_or(&0) as u32;
        let n = (b0 << 16) | (b1 << 8) | b2;
        out.push(T[((n >> 18) & 63) as usize] as char);
        out.push(T[((n >> 12) & 63) as usize] as char);
        if chunk.len() > 1 {
            out.push(T[((n >> 6) & 63) as usize] as char);
        }
        if chunk.len() > 2 {
            out.push(T[(n & 63) as usize] as char);
        }
    }
    out
}

fn hex(b: &[u8]) -> String {
    b.iter().map(|x| format!("{x:02x}")).collect()
}

/// 从 payload（JSON 片段）里取一个整数字段 —— 不引 serde，手写足够
fn json_u64(payload: &str, key: &str) -> Option<u64> {
    let pat = format!("\"{key}\"");
    let i = payload.find(&pat)? + pat.len();
    let rest = &payload[i..];
    let colon = rest.find(':')?;
    let tail = &rest[colon + 1..];
    let start = tail.find(|c: char| c.is_ascii_digit())?;
    let digits: String = tail[start..].chars().take_while(|c| c.is_ascii_digit()).collect();
    digits.parse().ok()
}

fn json_str<'a>(payload: &'a str, key: &str) -> Option<&'a str> {
    let pat = format!("\"{key}\"");
    let i = payload.find(&pat)? + pat.len();
    let rest = &payload[i..];
    let colon = rest.find(':')?;
    let tail = &rest[colon + 1..];
    let q = tail.find('"')?;
    let after = &tail[q + 1..];
    let end = after.find('"')?;
    Some(&after[..end])
}

/// 验签（供测试与 FFI 用）
pub fn verify(token: &str, now_unix: u64, device_fp: &[u8]) -> LicResult {
    let mut it = token.split('.');
    let (Some(prefix), Some(payload_b64), Some(sig_b64)) = (it.next(), it.next(), it.next()) else {
        return LicResult::Malformed;
    };
    if prefix != "YIEYU2" || it.next().is_some() {
        return LicResult::BadPrefix;
    }
    let Some(sig) = b64url_decode(sig_b64.as_bytes()) else { return LicResult::BadBase64 };
    let Some(payload) = b64url_decode(payload_b64.as_bytes()) else { return LicResult::BadBase64 };
    let Ok(payload) = std::str::from_utf8(&payload) else { return LicResult::Malformed };

    // ① 签名：对 "YIEYU2.<payload_b64>" 整段验
    let signed = format!("YIEYU2.{payload_b64}");
    let want = hmac_sha256(&HMAC_KEY, signed.as_bytes());
    if sig.len() != want.len() {
        return LicResult::BadSignature;
    }
    // 常数时间比较（不提前 return）
    let mut diff = 0u8;
    for (a, b) in sig.iter().zip(want.iter()) {
        diff |= a ^ b;
    }
    if diff != 0 {
        return LicResult::BadSignature;
    }

    // ② 过期
    if let Some(exp) = json_u64(payload, "e") {
        if exp != 0 && now_unix > exp {
            return LicResult::Expired;
        }
    }
    // ③ 设备绑定（可选字段）
    if let Some(d) = json_str(payload, "d") {
        if !device_fp.is_empty() && !d.is_empty() && d != hex(device_fp) {
            return LicResult::DeviceMismatch;
        }
    }
    LicResult::Ok
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

/// 自检：密钥是否已内嵌（返回 1 表示可用）
#[no_mangle]
pub extern "C" fn yieyu_license_has_pubkey() -> i32 {
    if HMAC_KEY.iter().all(|&b| b == 0) { 0 } else { 1 }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn sign(payload: &str) -> String {
        let p = b64url_encode(payload.as_bytes());
        let mac = hmac_sha256(&HMAC_KEY, format!("YIEYU2.{p}").as_bytes());
        format!("YIEYU2.{p}.{}", b64url_encode(&mac))
    }

    #[test]
    fn sha256_matches_known_vectors() {
        assert_eq!(hex(&sha256(b"")), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
        assert_eq!(hex(&sha256(b"abc")), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    }

    #[test]
    fn hmac_rfc4231_case1() {
        // RFC 4231 Test Case 1：key=0x0b×20, data="Hi There"
        let key = [0x0bu8; 20];
        assert_eq!(hex(&hmac_sha256(&key, b"Hi There")), "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
    }

    #[test]
    fn roundtrip_and_rejects() {
        let tok = sign("{\"u\":\"boss\",\"e\":9999999999}");
        assert!(matches!(verify(&tok, 1, &[]), LicResult::Ok));
        assert!(matches!(verify(&tok, 10_000_000_000, &[]), LicResult::Expired));
        assert!(matches!(verify("YIEYU2.a.b", 0, &[]), LicResult::BadBase64));
        assert!(matches!(verify("YIEYU1.x.y", 0, &[]), LicResult::BadPrefix));
        let mut bad = tok.clone();
        bad.push_str("x");
        assert!(!matches!(verify(&bad, 1, &[]), LicResult::Ok));
    }
}
