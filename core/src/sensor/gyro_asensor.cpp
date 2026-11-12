// [23] ASensor：纯 C++ 直接对话传感器，脑袋一转声音跟着转。
// 线程模型：ASensorEventQueue 回调在独立线程 → 只写原子快照，音频线程只读。
#include "gyro_asensor.h"
#include "../common/log.h"
#include <atomic>
#include <cmath>

#ifdef __ANDROID__
#include <android/sensor.h>
#include <android/looper.h>
#include <unistd.h>

namespace {
ASensorManager* g_mgr = nullptr;
ASensorEventQueue* g_q = nullptr;
ALooper* g_looper = nullptr;
const ASensor* g_rot = nullptr;
std::atomic<float> g_yaw{0}, g_pitch{0}, g_roll{0};
std::atomic<bool>  g_run{false};
std::atomic<int>   g_events{0};

int on_event(int, int, void* data) {
  ASensorEvent ev;
  ASensorEventQueue* q = (ASensorEventQueue*)data;
  while (ASensorEventQueue_getEvents(q, &ev, 1) > 0) {
    if (ev.type == ASENSOR_TYPE_ROTATION_VECTOR) {
      // 四元数 → yaw/pitch/roll（只做一次三角函数，200Hz 下可忽略）
      const float x = ev.data[0], y = ev.data[1], z = ev.data[2], w = ev.data[3];
      const float yaw = std::atan2(2.0f * (w * z + x * y), 1.0f - 2.0f * (y * y + z * z));
      const float sp  = 2.0f * (w * x - y * z);
      const float pitch = std::asin(sp > 1.f ? 1.f : (sp < -1.f ? -1.f : sp));
      g_yaw.store(yaw * 57.2957795f, std::memory_order_relaxed);
      g_pitch.store(pitch * 57.2957795f, std::memory_order_relaxed);
      g_events.fetch_add(1, std::memory_order_relaxed);
    }
  }
  return 1;
}
}  // namespace

extern "C" int gyro_start(int hz) {
  g_mgr = ASensorManager_getInstanceForPackage("com.yieyu.ultimate");
  if (!g_mgr) return -1;
  g_rot = ASensorManager_getDefaultSensor(g_mgr, ASENSOR_TYPE_ROTATION_VECTOR);
  if (!g_rot) { LOGW("[23] 本机无旋转矢量传感器 → 8D 头追降级为自动旋转"); return -2; }
  g_looper = ALooper_prepare(ALOOPER_PREPARE_ALLOW_NON_CALLBACKS);
  g_q = ASensorManager_createEventQueue(g_mgr, g_looper, 0, on_event, nullptr);
  ASensorEventQueue_setEventRate(g_q, g_rot, (int32_t)(1000000 / (hz > 0 ? hz : 200)));
  ASensorEventQueue_enableSensor(g_q, g_rot);
  g_run.store(true);
  LOGI("[23] ASensor 旋转矢量 @%d Hz（纯 C++，不经 Java）", hz);
  return 0;
}
extern "C" void gyro_stop(void) {
  if (!g_q) return;
  ASensorEventQueue_disableSensor(g_q, g_rot);
  ASensorManager_destroyEventQueue(g_mgr, g_q);
  g_q = nullptr; g_run.store(false);
}
extern "C" int gyro_read(float* y, float* p, float* r) {
  if (!g_run.load()) return -1;
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
