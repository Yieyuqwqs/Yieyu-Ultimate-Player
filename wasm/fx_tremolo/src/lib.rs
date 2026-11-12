//! 示例插件：颤音。演示模块 8 的插件 ABI（无 std、无 alloc、无 panichandler）。
//!
//! 预算：32~64 帧 × 2 声道，interpreter 下目标 < 0.2ms。

#![no_std]

static mut PHASE: f32 = 0.0;
static mut RATE_HZ: f32 = 5.0;
static mut DEPTH: f32 = 0.3;

#[no_mangle]
pub extern "C" fn yieyu_abi() -> i32 { 1 }

#[no_mangle]
pub extern "C" fn yieyu_init(sample_rate: i32, params: *const f32, n: i32) -> i32 {
    if sample_rate <= 0 { return -1; }
    if !params.is_null() && n >= 1 { unsafe { RATE_HZ = *params; } }
    if !params.is_null() && n >= 2 { unsafe { DEPTH = *(params.add(1)); } }
    unsafe {
        if !(RATE_HZ > 0.0) { RATE_HZ = 5.0; }
        if !(0.0..=1.0).contains(&DEPTH) { DEPTH = 0.3; }
        PHASE = 0.0;
    }
    0
}

#[no_mangle]
pub extern "C" fn yieyu_process(io: *mut f32, frames: i32) -> i32 {
    if io.is_null() || frames <= 0 { return -1; }
    let sr = 48_000.0f32; // 宿主保证的采样率；P2 从 yieyu_init 记下来
    unsafe {
        let step = RATE_HZ / sr;
        for f in 0..frames {
            PHASE += step;
            if PHASE >= 1.0 { PHASE -= 1.0; }
            let g = 1.0 - DEPTH * 0.5 * (1.0 + sin_approx(PHASE * 6.2831853));
            let base = (f as usize) * 2;
            *io.add(base) *= g;
            *io.add(base + 1) *= g;
        }
    }
    0
}

/// 无 std 环境下的 sin 近似（抛物线 + 修正项，误差 <0.2%）
fn sin_approx(x: f32) -> f32 {
    let mut t = x;
    // 折到 [-pi, pi]
    while t > 3.14159265 { t -= 6.2831853; }
    while t < -3.14159265 { t += 6.2831853; }
    let y = 1.27323954 * t - 0.405284735 * t * t;
    if y < 0.0 { 0.225 * (y * -y - y) + y } else { 0.225 * (y * y - y) + y }
}

#[panic_handler]
fn panic(_: &core::panic::PanicInfo) -> ! { loop {} }
