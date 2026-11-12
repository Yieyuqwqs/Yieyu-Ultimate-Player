// [23] ASensor：纯 C++ 直接对话传感器芯片，脑袋一转声音跟着转。
// 线程模型：ALooper 轮询线程 → 只写原子快照；音频线程只读，绝不回调进入。
#include "gyro_asensor.h"
#include "../common/log.h"
#include <atomic>
#include <cmath>
#include <thread>

#ifdef __ANDROID__
#include <android/looper.h>
#include <android/sensor.h>

namespace {
ASensorManager*   g_mgr = nullptr;
ASensorEventQueue* g_q = nullptr;
ALooper*          g_looper = nullptr;
const ASensor*    g_rot = nullptr;
std::atomic<float> g_yaw{0}, g_pitch{0}, g_roll{0};
std::atomic<bool>  g_run{false};
std::atomic<int>   g_events{0};
std::thread        g_thread;

// 返回 1 = 继续监听（ALooper_callbackFunc 契约）
int on_event(int, int, void* data) {
  auto* q = static_cast<ASensorEventQueue*>(data);
  ASensorEvent ev;
  while (ASensorEventQueue_getEvents(q, &ev, 1) > 0) {
    if (ev.type == ASENSOR_TYPE_ROTATION_VECTOR) {
      const float x = ev.data[0], y = ev.data[1], z = ev.data[2], w = ev.data[3];
      const float yaw = std::atan2(2.0f * (w * z + x * y), 1.0f - 2.0f * (y * y + z * z));
      float sp = 2.0f * (w * x - y * z);
      if (sp > 1.0f) sp = 1.0f;
      if (sp < -1.0f) sp = -1.0f;
      const float pitch = std::asin(sp);
      g_yaw.store(yaw * 57.2957795f, std::memory_order_relaxed);
      g_pitch.store(pitch * 57.2957795f, std::memory_order_relaxed);
      g_events.fetch_add(1, std::memory_order_relaxed);
    }
  }
  return 1;
}

void poll_loop() {
  while (g_run.load(std::memory_order_relaxed)) {
    // -1 毫秒 = 阻塞直到有事件（比 busy loop 省电，比 sleep 低延迟）
    ALooper_pollOnce(-1, nullptr, nullptr, nullptr);
  }
}
}  // namespace

extern "C" int gyro_start(int hz) {
  if (g_run.exchange(true)) return 0;
  g_mgr = ASensorManager_getInstanceForPackage("com.yieyu.ultimate");
  if (!g_mgr) { g_run.store(false); return -1; }
  g_rot = ASensorManager_getDefaultSensor(g_mgr, ASENSOR_TYPE_ROTATION_VECTOR);
  if (!g_rot) {
    LOGW("[23] 本机无旋转矢量传感器 → 8D 走自动旋转，不报错");
    g_run.store(false);
    return -2;
  }
  g_looper = ALooper_prepare(ALOOPER_PREPARE_ALLOW_NON_CALLBACKS);
  g_q = ASensorManager_createEventQueue(g_mgr, g_looper, 0, on_event, nullptr);
  ASensorEventQueue_setEventRate(g_q, g_rot, (int32_t)(1000000 / (hz > 0 ? hz : 200)));
  ASensorEventQueue_enableSensor(g_q, g_rot);
  g_thread = std::thread(poll_loop);
  LOGI("[23] ASensor 旋转矢量 @%d Hz（纯 C++，不经 Java）", hz);
  return 0;
}

extern "C" void gyro_stop(void) {
  if (!g_run.exchange(false)) return;
  if (g_thread.joinable()) g_thread.join();
  if (g_q) { ASensorEventQueue_disableSensor(g_q, g_rot); ASensorManager_destroyEventQueue(g_mgr, g_q); g_q = nullptr; }
}

extern "C" int gyro_read(float* y, float* p, float* r) {
  if (!g_run.load() && g_events.load() == 0) return -1;
  if (y) *y = g_yaw.load(std::memory_order_relaxed);
  if (p) *p = g_pitch.load(std::memory_order_relaxed);
  if (r) *r = g_roll.load(std::memory_order_relaxed);
  return 0;
}

extern "C" int gyro_is_available(void) { return g_rot ? 1 : 0; }

#else
extern "C" int  gyro_start(int hz) { (void)hz; return -1; }
extern "C" void gyro_stop(void) {}
extern "C" int  gyro_read(float* y, float* p, float* r) { (void)y; (void)p; (void)r; return -1; }
extern "C" int  gyro_is_available(void) { return 0; }
#endif
