// 总指挥。真正的"业务决策"只有三件：能不能播、播什么、出错怎么办。
#include "engine.h"
#include <cstdio>
#include "../jsb/jsb.h"
#include "../common/log.h"
#include "../common/blackbox.h"
#include "../sensor/gyro_asensor.h"
#include "../store/sqlite_kv.h"

namespace yieyu {

Engine& Engine::Instance() { static Engine e; return e; }

Engine::Engine()
    : byteRing_(new ByteRing(1u << 18)),   // 256 KiB 字节环（curl→ffmpeg，够 3s 预取）
      pcmRing_(new PcmRing(1u << 16)) {}   // 64K 样本 ≈ 1.3s @48k（DSP 与 AAudio 之间）

Engine::~Engine() = default;

int Engine::Boot(const char* filesDir, const char* abi) {
  // ⓪ 黑匣子先起来：之后任何模块出错才有地方落盘。
  //    首选 /storage/emulated/0/yieyu（用户能直接拿），无权限就降级到 App 私有目录。
  char fallback[600];
  std::snprintf(fallback, sizeof(fallback), "%s/yieyu", filesDir ? filesDir : ".");
  const int pub = bb_init("/storage/emulated/0/yieyu", fallback);
  if (pub != 0) {
    // 这条本身就值得记：说明用户没给全盘权限，导出按钮只能拿到私有目录
    bb_write(YIEYU_LOG_ERROR, "[0] 日志目录降级到私有目录：%s（公共目录不可写）", fallback);
  }

  state_.store(State::kLicensing);

  // ① [10] 验签：唯一密钥在这里，Kotlin/Dart/Lua 一律不持有
  //    （骨架期返回 OK；接了 license-ring 后换成真调用）
  const bool licensed = true;
  if (!licensed) {
    state_.store(State::kUnlicensed);
    bb_write(YIEYU_LOG_ERROR, "[10] 验签失败 → 熔断（停解码/清环/置灰），trace=%s", bb_trace());
    LOGW("license fail → 熔断：停解码、清 PCM 环、UI 置灰（不崩进程）");
    return 0;  // 注意：返回 0，UI 仍要起来（退款提示页）
  }

  // ② [20] 打开 SQLite（唯一写者，WAL）
  if (sqlite_kv_open(filesDir) != 0) {
    state_.store(State::kFault);
    bb_write(YIEYU_LOG_FATAL, "[20] SQLite 打开失败，dir=%s", filesDir ? filesDir : "?");
    return -2;
  }
  sqlite_kv_load_session();

  // ③ [23] 陀螺仪（8D 用；没有传感器就静默降级，不外抛）
  gyro_start(200 /* Hz */);

  state_.store(State::kReady);
  LOGI("engine ready abi=%s sr=%d", abi, sampleRate_.load());
  return 0;
}

void Engine::Shutdown() {
  gyro_stop();
  sqlite_kv_close();
  bb_flush();
  bb_shutdown();
  state_.store(State::kIdle);
}

int Engine::HandleCommand(const uint8_t* fbs, size_t len) {
  // 骨架期：只做"长度/版本"校验，真实分发见 idl/jsb.fbs 的 op 枚举
  if (len < 8) return -1;
  const uint8_t op = fbs[4];  // FlatBuffers 里 vtable 之后首个字段（示意）
  switch (op) {
    case 1: state_.store(State::kPlaying); LOGI("cmd: play"); return 0;
    case 2: state_.store(State::kReady);   LOGI("cmd: pause"); return 0;
    default: LOGI("cmd op=%u len=%zu（未实现）", op, len); return 0;
  }
}

size_t Engine::DrainEvents(uint8_t* out, size_t cap) {
  (void)out; (void)cap;
  return 0;  // TODO(P1)：事件队列 → FlatBuffers 打包
}

int Engine::Snapshot(Yieyu_Snapshot* out) {
  out->playing   = state_.load() == State::kPlaying ? 1 : 0;
  out->sampleRate = sampleRate_.load();
  out->positionMs = positionMs_.load();
  out->durationMs = durationMs_.load();
  out->peakL = peakL_.load();
  out->peakR = peakR_.load();
  out->eqPoints = 31;  // [17] 31 段 EQ → [2] Rive 曲线
  return 0;
}

void Engine::OnSystemAction(int actionId) {
  // 模块22 只转发，不做判定
  LOGI("system action %d", actionId);
}

void Engine::Stats(char* out, size_t cap) {
  std::snprintf(out, cap,
                "state=%d sr=%d pos=%lld | jsb=ok dsp=ok sensor=ok store=ok",
                (int)state_.load(), sampleRate_.load(), (long long)positionMs_.load());
}

}  // namespace yieyu
