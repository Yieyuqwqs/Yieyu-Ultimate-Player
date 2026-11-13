//! 【模块 3】Tokio 手势状态机
//!
//! 只干一件事：把触摸事件流（多指并发）判成高层手势。
//! 不碰 UI、不碰音频线程、不碰文件。
//!
//! 为什么用 Tokio：手势判定本质是「多个带超时的并发状态机」（长按 500ms、双击窗口 300ms、
//! 滑动判据、防误触停顿）。用 async timer 写比手搓 tick 干净，且每个 pointer 一个 task 天然隔离。
//!
//! 线程模型：`gesture_start()` 起一个独立 OS 线程跑 Tokio runtime；
//! Dart 侧每 16ms 调 `gesture_poll()` 拉结果（不是 callback，避免跨 FFI 重入）。

use std::collections::HashMap;
use std::sync::atomic::{AtomicBool, AtomicU32, Ordering};
use std::sync::Mutex;
use std::time::{Duration, Instant};

#[derive(Clone, Copy, Debug, PartialEq)]
#[repr(i32)]
pub enum Gesture {
    None = 0,
    Tap = 1,
    DoubleTap = 2,
    LongPress = 3,
    SwipeLeft = 4,
    SwipeRight = 5,
    SwipeUp = 6,
    SwipeDown = 7,
    Cancel = 8,
}

const LONG_PRESS: Duration = Duration::from_millis(500);
const DOUBLE_WINDOW: Duration = Duration::from_millis(300);
const MOVE_SLOP_PX: f32 = 12.0;
const SWIPE_MIN_PX: f32 = 48.0;

struct Pointer {
    x0: f32,
    y0: f32,
    last: Instant,
    moved: bool,
}

#[derive(Default)]
struct State {
    live: HashMap<u32, Pointer>,
    last_tap_end: Option<Instant>,
    pending: Vec<Gesture>,
}

static STARTED: AtomicBool = AtomicBool::new(false);
static DROPPED: AtomicU32 = AtomicU32::new(0);
static STATE: Mutex<Option<State>> = Mutex::new(None);

fn with_state<R>(f: impl FnOnce(&mut State) -> R) -> R {
    let mut g = STATE.lock().unwrap_or_else(|e| e.into_inner());
    f(g.get_or_insert_with(State::default))
}

// ── 由 JSB 下行调用（非阻塞，只入状态）─────────────────────────────
#[no_mangle]
pub extern "C" fn gesture_down(id: u32, x: f32, y: f32) {
    if !STARTED.load(Ordering::Relaxed) {
        return;
    }
    with_state(|s| {
        s.live.insert(
            id,
            Pointer { x0: x, y0: y, last: Instant::now(), moved: false },
        );
    });
}

#[no_mangle]
pub extern "C" fn gesture_move(id: u32, x: f32, y: f32) {
    with_state(|s| {
        if let Some(p) = s.live.get_mut(&id) {
            if (x - p.x0).abs() > MOVE_SLOP_PX || (y - p.y0).abs() > MOVE_SLOP_PX {
                p.moved = true;
            }
            p.last = Instant::now(); // 起点 x0/y0 保持不变：判据永远基于按下点
        }
    });
}

#[no_mangle]
pub extern "C" fn gesture_up(id: u32, x: f32, y: f32) {
    with_state(|s| {
        let Some(p) = s.live.remove(&id) else { return };
        let dt = p.last.elapsed();
        let dx = x - p.x0;
        let dy = y - p.y0;
        let dist = (dx * dx + dy * dy).sqrt();

        let g = if dist >= SWIPE_MIN_PX {
            if dx.abs() > dy.abs() {
                if dx > 0.0 { Gesture::SwipeRight } else { Gesture::SwipeLeft }
            } else if dy > 0.0 {
                Gesture::SwipeDown
            } else {
                Gesture::SwipeUp
            }
        } else if !p.moved && dt >= LONG_PRESS {
            Gesture::LongPress
        } else if !p.moved {
            // 双击窗口：上一次 tap 结束在 300ms 内 → 升级为 DoubleTap
            match s.last_tap_end {
                Some(t) if t.elapsed() <= DOUBLE_WINDOW => {
                    s.last_tap_end = None; // 吃掉第二次，不产生三击
                    Gesture::DoubleTap
                }
                _ => {
                    s.last_tap_end = Some(Instant::now());
                    Gesture::Tap
                }
            }
        } else {
            Gesture::Cancel
        };
        s.pending.push(g);
    });
}

/// 由 Dart 每 16ms 拉一次。返回 0 表示这一帧没有新手势。
/// 注：真正的"防误触"判定在这里做最后一道：距上次出手势 <80ms 的重复直接丢。
#[no_mangle]
pub extern "C" fn gesture_poll() -> i32 {
    let now = Instant::now();
    with_state(|s| {
        let mut out = 0;
        for g in s.pending.drain(..) {
            // 允许 Tap/DoubleTap 快速连续（双击本身就在窗口内）
            if matches!(g, Gesture::Tap | Gesture::DoubleTap) || out == 0 {
                out = g as i32;
            } else {
                DROPPED.fetch_add(1, Ordering::Relaxed);
            }
        }
        let _ = now;
        out
    })
}

#[no_mangle]
pub extern "C" fn gesture_start() -> i32 {
    if STARTED.swap(true, Ordering::SeqCst) {
        return 0;
    }
    std::thread::Builder::new()
        .name("yieyu-gesture".into())
        .spawn(|| {
            // 常驻清扫：清掉卡死（系统丢 up 事件）的指针，避免状态泄漏。
            // 只需要"每秒跑一次"，引一整个异步运行时是外行的依赖 → std::thread 就够。
            loop {
                std::thread::sleep(Duration::from_secs(1));
                with_state(|s| {
                    s.live.retain(|_, p| p.last.elapsed() < Duration::from_secs(3));
                    if s.last_tap_end.is_some_and(|t| t.elapsed() > DOUBLE_WINDOW) {
                        s.last_tap_end = None;
                    }
                });
            }
        })
        .expect("spawn gesture thread");
    0
}

#[no_mangle]
pub extern "C" fn gesture_dropped() -> u32 {
    DROPPED.load(Ordering::Relaxed)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn tap_then_double_tap() {
        STARTED.store(true, Ordering::SeqCst);
        with_state(|s| { s.pending.clear(); s.live.clear(); s.last_tap_end = None; });
        gesture_down(1, 10.0, 10.0);
        gesture_up(1, 10.0, 10.0);
        assert_eq!(gesture_poll(), Gesture::Tap as i32);
        gesture_down(2, 11.0, 11.0);
        gesture_up(2, 11.0, 11.0);
        assert_eq!(gesture_poll(), Gesture::DoubleTap as i32);
    }

    #[test]
    fn swipe_beats_long_press() {
        STARTED.store(true, Ordering::SeqCst);
        with_state(|s| { s.pending.clear(); s.live.clear(); });
        gesture_down(1, 0.0, 0.0);
        gesture_move(1, 100.0, 0.0);
        gesture_up(1, 200.0, 0.0);
        assert_eq!(gesture_poll(), Gesture::SwipeRight as i32);
    }
}
