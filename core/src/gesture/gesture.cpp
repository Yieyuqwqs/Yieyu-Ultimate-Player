// [3] 手势状态机（零依赖：不编 Rust/Tokio，直接链进 core）
//
// 原实现是 Rust + Tokio，但它唯一做的事就是"按坐标与时间判一个手势"——
// 引一整条异步运行时链是外行的依赖。这里是同样的判定规则，纯 C++ 单锁。
//
// 返回码（Dart 侧一致）：0 None / 1 Tap / 2 DoubleTap / 3 LongPress
//                      4 SwipeLeft / 5 SwipeRight / 6 SwipeUp / 7 SwipeDown / 8 Cancel
#include <stdint.h>
#include <mutex>
#include <chrono>

namespace {
std::mutex g_mu;
double g_x = 0, g_y = 0, g_dx = 0, g_dy = 0;
bool g_down = false;
int64_t g_lastTapMs = -10000;
int g_queue = 0;          // 待取走的判定结果
bool g_hasEvent = false;
constexpr double kSwipeMin = 48.0;    // dp
constexpr int64_t kTapMaxMs = 300;
constexpr int64_t kLongMinMs = 550;
constexpr int64_t kDoubleWindowMs = 300;

int64_t nowMs() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}
int64_t g_downMs = 0;

void emit(int code) {
  g_queue = code;
  g_hasEvent = true;
}
}  // namespace

#if defined(__GNUC__) || defined(__clang__)
#define YP_EXPORT __attribute__((visibility("default")))   // 全库 -fvisibility=hidden，导出必须显式标
#else
#define YP_EXPORT
#endif

extern "C" {

YP_EXPORT int gesture_start(void) { return 0; }

YP_EXPORT void gesture_down(uint32_t pointer, float x, float y) {
  std::lock_guard<std::mutex> lk(g_mu);
  (void)pointer;
  g_down = true;
  g_x = x;
  g_y = y;
  g_dx = 0;
  g_dy = 0;
  g_downMs = nowMs();
}

YP_EXPORT void gesture_move(uint32_t pointer, float x, float y) {
  std::lock_guard<std::mutex> lk(g_mu);
  (void)pointer;
  if (!g_down) return;
  g_dx = x - g_x;
  g_dy = y - g_y;
}

YP_EXPORT void gesture_up(uint32_t pointer, float x, float y) {
  std::lock_guard<std::mutex> lk(g_mu);
  (void)pointer;
  if (!g_down) return;
  g_down = false;
  g_dx = x - g_x;
  g_dy = y - g_y;
  const int64_t held = nowMs() - g_downMs;
  const double ax = g_dx < 0 ? -g_dx : g_dx;
  const double ay = g_dy < 0 ? -g_dy : g_dy;

  if (ax > kSwipeMin || ay > kSwipeMin) {
    if (ax >= ay) emit(g_dx > 0 ? 5 : 4);        // 右/左
    else emit(g_dy > 0 ? 7 : 6);                 // 下/上
    return;
  }
  if (held >= kLongMinMs) { emit(3); return; }
  if (held <= kTapMaxMs) {
    const int64_t now = nowMs();
    if (now - g_lastTapMs <= kDoubleWindowMs) {
      g_lastTapMs = -10000;
      emit(2);
    } else {
      g_lastTapMs = now;
      emit(1);
    }
    return;
  }
  emit(0);
}

YP_EXPORT int gesture_poll(void) {
  std::lock_guard<std::mutex> lk(g_mu);
  if (!g_hasEvent) return 0;
  g_hasEvent = false;
  const int r = g_queue;
  g_queue = 0;
  return r;
}

YP_EXPORT int gesture_alive(void) { return 1; }

}  // extern "C"
