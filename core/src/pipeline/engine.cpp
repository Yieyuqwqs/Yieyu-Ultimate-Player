// 总指挥。真正的"业务决策"只有三件：能不能播、播什么、出错怎么办。
#include "engine.h"
#include <cstdio>
#include "../jsb/jsb.h"
#include "../common/log.h"
#include "../common/blackbox.h"
#include "../common/app_paths.h"
#include "../sensor/gyro_asensor.h"
#include "../store/sqlite_kv.h"
#include "player.h"
#include "../audio/aaudio_sink.h"

namespace yieyu {

Engine& Engine::Instance() { static Engine e; return e; }

Engine::Engine()
    : byteRing_(new ByteRing(1u << 18)) {}   // 256 KiB 字节环（[9] 拉流用，3s 预取）

Engine::~Engine() = default;

PcmRing& Engine::pcmRing() { return Player::Instance().Ring(); }

int Engine::Boot(const char* filesDir, const char* abi) {
  // ⓪ 路径先定死（唯一真相），再起黑匣子：之后任何模块要写盘都问 app_paths
  const int pub = yp_init(filesDir);
  if (bb_init(yp_home()) != 0) {
    // 连私有目录都写不进：没有日志可落，但 App 仍要起来（UI 静默，原因在 dmesg 之外看不到）
  }
  if (!pub) {
    bb_write(YIEYU_LOG_ERROR, "[0] 公共目录不可写，运行目录落在私有目录：%s", yp_home());
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
  if (sqlite_kv_open(yp_data()) != 0) {
    state_.store(State::kFault);
    bb_write(YIEYU_LOG_FATAL, "[20] SQLite 打开失败，dir=%s", filesDir ? filesDir : "?");
    return -2;
  }
  sqlite_kv_load_session();

  // ③ [23] 陀螺仪（8D 用；没有传感器就静默降级，不外抛）
  gyro_start(200 /* Hz */);

  state_.store(State::kReady);
  LOGI("engine ready abi=%s sr=%d", abi, Player::Instance().SampleRate());
  return 0;
}

void Engine::Shutdown() {
  Player::Instance().Shutdown();
  gyro_stop();
  sqlite_kv_close();
  bb_flush();
  bb_shutdown();
  state_.store(State::kIdle);
}

int Engine::PlayUrl(const char* uri) { return Player::Instance().PlayUrl(uri); }
void Engine::PauseAudio() { Player::Instance().Pause(); }
void Engine::ResumeAudio() { Player::Instance().Resume(); }
void Engine::StopAudio() { Player::Instance().Stop(); }
int  Engine::SeekMs(int64_t ms) { return Player::Instance().SeekMs(ms); }
void Engine::SetVolume(float v) { Player::Instance().SetVolume(v); }
void Engine::PlayerError(char* out, size_t cap) const { Player::Instance().LastError(out, cap); }
void Engine::SetDsp(bool on) { Player::Instance().SetDsp(on); }
void Engine::SetEqGain(int band, float db) { Player::Instance().SetEqGain(band, db); }
void Engine::SetTone(bool tube, bool vinyl, bool lpf, float drive) {
  Player::Instance().SetTone(tube, vinyl, lpf, drive);
}

int Engine::HandleCommand(const uint8_t* fbs, size_t len) {
  // 骨架期：只做"长度/版本"校验，真实分发见 idl/jsb.fbs 的 op 枚举
  if (len < 8) return -1;
  const uint8_t op = fbs[4];  // FlatBuffers 里 vtable 之后首个字段（示意）
  switch (op) {
    case 1: ResumeAudio(); return 0;   // play
    case 2: PauseAudio();  return 0;   // pause
    case 3: StopAudio();   return 0;   // stop
    default: LOGI("cmd op=%u len=%zu（未实现）", op, len); return 0;
  }
}

size_t Engine::DrainEvents(uint8_t* out, size_t cap) {
  (void)out; (void)cap;
  return 0;  // TODO(P1)：事件队列 → FlatBuffers 打包（搜索结果/错误/授权状态）
}

int Engine::Snapshot(Yieyu_Snapshot* out) {
  Player& p = Player::Instance();
  out->playing    = p.IsPlaying() ? 1 : 0;
  out->sampleRate = p.SampleRate();
  out->positionMs = p.PositionMs();
  out->durationMs = p.DurationMs();
  out->peakL      = p.PeakL();
  out->peakR      = p.PeakR();
  out->eqPoints   = 31;  // [17] 31 段 EQ → [2] Rive 曲线
  return 0;
}

void Engine::OnSystemAction(int actionId) {
  // 模块22 只转发，不做判定
  switch (actionId) {
    case 0: ResumeAudio(); break;   // play
    case 1: PauseAudio();  break;   // pause
    default: break;                 // next/prev 由上层（模块 1）决定播什么
  }
}

void Engine::Stats(char* out, size_t cap) {
  Player& p = Player::Instance();
  std::snprintf(out, cap,
                "state=%d sr=%d pos=%lld dur=%lld st=%d underrun=%d | jsb=ok dsp=ok sensor=ok store=ok",
                (int)state_.load(), (int)p.SampleRate(), (long long)p.PositionMs(),
                (long long)p.DurationMs(), p.State(), (int)yieyu_aaudio_underruns());
}

}  // namespace yieyu
