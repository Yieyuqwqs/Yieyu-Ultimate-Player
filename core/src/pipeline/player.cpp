// 播放调度实现。见 player.h 的线程模型；本文件是"能放歌"的全部逻辑。
#include "player.h"

#include <algorithm>
#include <chrono>
#include <cstring>

#include "../audio/aaudio_sink.h"
#include "../audio/demux_platform.h"
#include "../common/log.h"

namespace yieyu {
namespace {

constexpr int    kChunkFrames = 2048;              // 每次解码的立体声帧数
constexpr int    kRingFloats  = 1 << 17;           // 131072 float ≈ 65536 帧 ≈ 1.4s @44.1k
constexpr size_t kBacklog     = (size_t)kRingFloats / 2;   // 环过半就等音频线程消费

float g_scratch[kChunkFrames * 2];                 // 解码线程专用暂存

int pull_trampoline(void* ctx, float* out, int frames) {
  return static_cast<Player*>(ctx)->Pull(out, frames);
}

inline void sleep_ms(int ms) {
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

}  // namespace

Player& Player::Instance() {
  static Player p;
  return p;
}

Player::Player() : ring_(new PcmRing((size_t)kRingFloats)) {}

Player::~Player() = default;

// ── 控制面 ────────────────────────────────────────────────────────────
int Player::PlayUrl(const char* uri) {
  if (!uri || !*uri) return -1;
  {
    std::lock_guard<std::mutex> lk(mu_);
    pendingUri_.assign(uri);
    ++gen_;
    stopReq_ = false;
    EnsureThreadLocked();
  }
  cv_.notify_all();
  return 0;
}

void Player::Pause() {
  int expect = kPlaying;
  state_.compare_exchange_strong(expect, kPaused);
}

void Player::Resume() {
  int expect = kPaused;
  state_.compare_exchange_strong(expect, kPlaying);
}

void Player::Stop() {
  {
    std::lock_guard<std::mutex> lk(mu_);
    stopReq_ = true;
    ++gen_;
    pendingUri_.clear();
  }
  state_.store(kIdle);
  cv_.notify_all();
}

int Player::SeekMs(int64_t ms) {
  if (ms < 0) return -1;
  const int st = State();
  if (st == kIdle || st == kError) return -2;   // 没在放，无事可做
  seekReqMs_.store(ms);
  return 0;
}

void Player::SetVolume(float v) {
  if (!(v >= 0.0f)) v = 0.0f;      // NaN 也当 0
  if (v > 1.0f) v = 1.0f;
  volume_.store(v, std::memory_order_relaxed);
}

void Player::Shutdown() {
  {
    std::lock_guard<std::mutex> lk(mu_);
    quit_ = true;
    stopReq_ = true;
    ++gen_;
  }
  cv_.notify_all();
  state_.store(kIdle);
  // 设备（sink / 解码器）一律让**解码线程自己**收尾：控制线程这时去 close，
  // 会和工作线程抢同一批原生句柄。线程可能正阻塞在网络打开上，所以只 detach 不 join
  // （进程马上退出，OS 会收走）。
  if (th_.joinable()) th_.detach();
}

int64_t Player::PositionMs() const {
  const int32_t rate = rate_.load(std::memory_order_relaxed);
  const int64_t base = baseMs_.load(std::memory_order_relaxed);
  const int64_t frames = playedFrames_.load(std::memory_order_relaxed);
  int64_t pos = base + (rate > 0 ? frames * 1000 / rate : 0);
  const int64_t dur = durationMs_.load(std::memory_order_relaxed);
  if (dur > 0 && pos > dur) pos = dur;
  return pos < 0 ? 0 : pos;
}

// ── 数据面：音频线程 ──────────────────────────────────────────────────
int Player::Pull(float* out, int frames) {
  if (!out || frames <= 0) return 0;
  if (State() != kPlaying) return 0;            // 暂停/换曲/出错 → 交给 sink 填静音
  size_t want = (size_t)frames * 2;
  size_t got = ring_->read(out, want);
  if (got & 1u) got -= 1;                       // 防御：绝不让左右声道错位半格
  const int produced = (int)(got / 2);
  if (produced <= 0) return 0;
  playedFrames_.fetch_add(produced, std::memory_order_relaxed);

  const float g = volume_.load(std::memory_order_relaxed);
  float peakL = 0.0f, peakR = 0.0f;
  for (size_t i = 0; i < got; i += 2) {
    float l = out[i], r = out[i + 1];
    if (g < 0.999f) {
      l *= g;
      r *= g;
      out[i] = l;
      out[i + 1] = r;
    }
    const float al = l < 0 ? -l : l;
    const float ar = r < 0 ? -r : r;
    if (al > peakL) peakL = al;
    if (ar > peakR) peakR = ar;
  }
  peakL_.store(peakL, std::memory_order_relaxed);
  peakR_.store(peakR, std::memory_order_relaxed);
  return produced;
}

// ── 解码线程 ──────────────────────────────────────────────────────────
void Player::EnsureThreadLocked() {
  if (th_.joinable()) return;
  th_ = std::thread([this] { WorkerLoop(); });
}

void Player::WorkerLoop() {
  for (;;) {
    std::string uri;
    uint64_t myGen = 0;
    {
      std::unique_lock<std::mutex> lk(mu_);
      cv_.wait(lk, [this] { return quit_ || !pendingUri_.empty(); });
      if (quit_) return;
      uri.swap(pendingUri_);
      myGen = gen_;
    }
    OpenAndPlay(uri, myGen);
  }
}

void Player::OpenAndPlay(const std::string& uri, uint64_t gen) {
  state_.store(kPreparing);
  StopSink();
  DrainRing();
  baseMs_.store(0);
  playedFrames_.store(0);
  durationMs_.store(0);
  seekReqMs_.store(-1);

  int32_t rate = 48000, ch = 2;
  int64_t durUs = 0;
  const int rc = yieyu_pdec_open(uri.c_str(), &rate, &ch, &durUs);
  if (rc != 0) {
    LOGE("[13] 打开音频失败 rc=%d uri=%.180s", rc, uri.c_str());
    state_.store(kError);
    return;
  }
  rate_.store(rate > 0 ? rate : 48000, std::memory_order_relaxed);
  durationMs_.store(durUs > 0 ? durUs / 1000 : 0, std::memory_order_relaxed);

  if (yieyu_aaudio_start(rate_.load(std::memory_order_relaxed), 192, &pull_trampoline, this) != 0) {
    LOGE("[15] AAudio 起不来 rate=%d", (int)rate_.load());
    yieyu_pdec_close();
    state_.store(kError);
    return;
  }
  sinkUp_.store(true, std::memory_order_relaxed);
  state_.store(kPlaying);

  for (;;) {
    // ① 换曲 / 停止 / 退出
    {
      std::lock_guard<std::mutex> lk(mu_);
      if (quit_ || stopReq_ || gen_ != gen) break;
    }
    // ② 暂停：不填环（环里的存量留着，恢复时无缝续上）
    if (State() == kPaused) { sleep_ms(5); continue; }
    // ③ seek 请求
    const int64_t reqMs = seekReqMs_.exchange(-1, std::memory_order_relaxed);
    if (reqMs >= 0) { DoSeek(reqMs); continue; }
    // ④ 背压：环里够多就等
    if (ring_->readable() > kBacklog) { sleep_ms(3); continue; }
    // ⑤ 解一块
    const int n = yieyu_pdec_read(g_scratch, kChunkFrames);
    if (n > 0) {
      ring_->write(g_scratch, (size_t)n * 2);
      continue;
    }
    if (yieyu_pdec_eos()) {
      // 播完：等环里剩下的全部播出去再收工（否则最后 1 秒会被切掉）
      if (ring_->readable() == 0) break;
      sleep_ms(10);
      continue;
    }
    sleep_ms(2);   // 解码器还在要数据
  }

  StopSink();
  yieyu_pdec_close();
  state_.store(kIdle);
}

void Player::DoSeek(int64_t ms) {
  const int prev = State();
  // 先让音频回调停手（它看到 !kPlaying 就只出静音，不碰环），再动环 → 守住 SPSC
  state_.store(kPreparing);
  sleep_ms(20);
  DrainRing();
  if (yieyu_pdec_seek(ms * 1000) != 0) {
    LOGE("[13] seek 失败 %lld ms", (long long)ms);
  }
  baseMs_.store(ms, std::memory_order_relaxed);
  playedFrames_.store(0, std::memory_order_relaxed);
  peakL_.store(0.0f, std::memory_order_relaxed);
  peakR_.store(0.0f, std::memory_order_relaxed);
  state_.store(prev == kPaused ? kPaused : kPlaying);
}

void Player::StopSink() {
  if (sinkUp_.exchange(false, std::memory_order_relaxed)) {
    yieyu_aaudio_stop();
  }
}

void Player::DrainRing() {
  while (ring_->readable() > 0) {
    const size_t got = ring_->read(g_scratch, (size_t)kChunkFrames * 2);
    if (got == 0) break;
  }
}

}  // namespace yieyu
