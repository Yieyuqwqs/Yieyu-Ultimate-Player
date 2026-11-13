// 播放调度实现。见 player.h 的线程模型；本文件是"能放歌"的全部逻辑。
#include "player.h"

#include <algorithm>
#include <chrono>
#include <cstring>

#include "../audio/aaudio_sink.h"
#include "../audio/demux_platform.h"
#include "../audio/dsd.h"          // [16] DSD → PCM
#include "../audio/resampler.h"    // [14] 多相重采样
#include "../dsp/dsp.h"
#include "../sensor/gyro_asensor.h"   // [23] 姿态 → [17] 8D
#include "../store/kv_store.h"        // [20] 播放进度落盘
#include "../common/log.h"
#include "../common/blackbox.h"
#include "../net/song_uri.h"   // yieyu://song?... → 真直链

namespace yieyu {
namespace {

constexpr int    kChunkFrames = 2048;              // 每次产出的立体声帧数（输出侧）
constexpr int    kMaxSrcFrames = 8192;             // 源侧一次最多读多少帧（重采样前）
constexpr int    kRingFloats  = 1 << 17;           // 131072 float ≈ 65536 帧 ≈ 1.4s @44.1k
constexpr size_t kBacklog     = (size_t)kRingFloats / 2;   // 环过半就等音频线程消费

float g_scratch[kChunkFrames * 2 + 1024];          // 解码线程专用暂存（输出域）
float g_srcscratch[kMaxSrcFrames * 2];             // [14] 重采样前的源域暂存

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

void Player::LastError(char* out, size_t cap) const {
  if (!out || cap == 0) return;
  std::lock_guard<std::mutex> lk(errMu_);
  std::snprintf(out, cap, "%s", err_);
}

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

void Player::SetTargetRate(int hz) {
  if (hz != 0 && hz != 44100 && hz != 48000 && hz != 88200 && hz != 96000 && hz != 176400 && hz != 192000) return;
  targetRate_.store(hz, std::memory_order_relaxed);
}

void Player::SetDsp(bool on) { dspOn_.store(on, std::memory_order_relaxed); }

bool Player::DspOn() const { return dspOn_.load(std::memory_order_relaxed); }

void Player::SetEqGain(int band, float db) {
  if (band < 0 || band >= 31) return;
  if (!(db >= -24.0f)) db = -24.0f;      // NaN / 过小都夹
  if (db > 24.0f) db = 24.0f;
  eq31_set_gain(band, db);
}

void Player::SetTone(bool tube, bool vinyl, bool lpf, float drive) {
  tube_vinyl_set(tube, vinyl, lpf, drive);
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
  wfail_.store(0, std::memory_order_relaxed);
  chunks_.store(0, std::memory_order_relaxed);

  // 内部 URI（yieyu://song?source=..&id=..）在这里换成真直链。
  // 网络往返发生在**解码线程**，控制面不等；失败原因直接进 LastError 给面板。
  char target[2048];
  if (yieyu_resolve_uri(uri.c_str(), target, sizeof(target)) != 0) {
    const char* why = yieyu_song_last_error();
    LOGE("[9] 直链解析失败：%s（%.120s）", why ? why : "?", uri.c_str());
    {
      std::lock_guard<std::mutex> lk(errMu_);
      std::snprintf(err_, sizeof(err_), "[9] 换不到播放地址：%s", why && *why ? why : "网络/接口失败");
    }
    state_.store(kError);
    return;
  }

  int32_t rate = 48000, ch = 2;
  int64_t durUs = 0;
  int32_t srcRate = 48000;
  const int wantRate = targetRate_.load(std::memory_order_relaxed);
  const int isDsf = dsd_probe(target);
  dsd_.store(isDsf == 1, std::memory_order_relaxed);
  resamp_.store(false, std::memory_order_relaxed);
  int rc = 0;
  if (isDsf == 1) {
    // [16] DSD：固定解到 192k（除非用户显式指定），中间率由模块自己算
    const int dsdOut = wantRate > 0 ? wantRate : 192000;
    int outCh = 2;
    rc = dsd_open(target, dsdOut, &rate, &outCh, &durUs);
    srcRate = rate;
    if (rc == 0) ch = outCh;
  } else {
    rc = yieyu_pdec_open(target, &rate, &ch, &durUs);
    srcRate = rate;
    if (rc == 0 && wantRate > 0 && wantRate != (int)rate) {
      // [14] 升/降频：目标率固定，DSP 与声卡都按目标率跑
      if (resample_init((int)rate, wantRate, ch) == 0) {
        rate = wantRate;
        resamp_.store(true, std::memory_order_relaxed);
      }
    }
  }
  if (rc != 0) {
    const char* why = (isDsf == 1) ? dsd_error() : yieyu_pdec_error();
    LOGE("[13/16] 打开音频失败 rc=%d uri=%.180s（%s）", rc, target, why ? why : "?");
    {
      std::lock_guard<std::mutex> lk(errMu_);
      std::snprintf(err_, sizeof(err_), "%s 打不开音频 rc=%d：%s", (isDsf == 1) ? "[16]" : "[13]", rc,
                    (why && *why) ? why : "未知原因");
    }
    state_.store(kError);
    return;
  }
  rate_.store(rate > 0 ? rate : 48000, std::memory_order_relaxed);
  durationMs_.store(durUs > 0 ? durUs / 1000 : 0, std::memory_order_relaxed);

  const int sinkRc = yieyu_aaudio_start(rate_.load(std::memory_order_relaxed), 192, &pull_trampoline, this);
  if (sinkRc != 0) {
    LOGE("[15] AAudio 起不来 rate=%d rc=%d", (int)rate_.load(), sinkRc);
    if (isDsf == 1) dsd_close();
    else yieyu_pdec_close();
    {
      std::lock_guard<std::mutex> lk(errMu_);
      std::snprintf(err_, sizeof(err_), "[15] 声卡打不开 rc=%d（%d Hz）", sinkRc, (int)rate_.load());
    }
    state_.store(kError);
    return;
  }
  sinkUp_.store(true, std::memory_order_relaxed);

  // [17] DSP 链按【源采样率】初始化；默认旁路（dspOn_ == false 时一个样本都不碰）
  eq31_init(rate_.load(std::memory_order_relaxed));
  tube_vinyl_init(rate_.load(std::memory_order_relaxed));
  hrtf8d_init(rate_.load(std::memory_order_relaxed));
  dspOn_.store(false, std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> lk(errMu_);
    err_[0] = 0;
  }

  state_.store(kPlaying);

  int64_t savedMs = -1;
  for (;;) {
    // ⓪ [23]→[17]：每块读一次姿态快照（非阻塞原子读），头追/自动旋转共用同一入口
    {
      float yaw = 0.0f, pitch = 0.0f, roll = 0.0f;
      if (gyro_read(&yaw, &pitch, &roll) == 0) hrtf8d_set_pose(yaw, pitch, 0);
    }
    // ⓪ [20] 播放进度：最多每 2 秒写一次（写者是 M20，音频线程一行都不碰）
    {
      const int64_t nowMs = PositionMs();
      if (nowMs / 2000 != savedMs / 2000) {
        savedMs = nowMs;
        char buf[32];
        const int n = std::snprintf(buf, sizeof(buf), "%lld", (long long)nowMs);
        kv_put("pos", buf, n);
      }
    }
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
    // ④ 背压：环里够多就等；空间不够一整块就先不解（否则会写出半块 → 左右声道错位）
    if (ring_->readable() > kBacklog) { sleep_ms(3); continue; }
    const size_t want = (size_t)kChunkFrames * 2;
    if (ring_->writable() < want) { sleep_ms(2); continue; }
    // ⑤ 解一块（DSD 直接出目标率；PCM 需要时先出源域再 [14] 转）
    int n = 0;
    if (isDsf == 1) {
      n = dsd_read(g_scratch, kChunkFrames);
    } else if (resamp_.load(std::memory_order_relaxed)) {
      int srcWant = (int)(((int64_t)kChunkFrames * srcRate) / (rate > 0 ? rate : 48000));
      if (srcWant < 64) srcWant = 64;
      if (srcWant > kMaxSrcFrames) srcWant = kMaxSrcFrames;
      const int sN = yieyu_pdec_read(g_srcscratch, srcWant);
      if (sN > 0) n = resample_process(g_srcscratch, sN, g_scratch, kChunkFrames + 512);
    } else {
      n = yieyu_pdec_read(g_scratch, kChunkFrames);
    }
    if (n > 0) {
      // [17] 音效链：只在解码线程跑（音频线程零处理，守铁律 2）
      if (dspOn_.load(std::memory_order_relaxed)) {
        eq31_process(g_scratch, n);
        tube_vinyl_process(g_scratch, n);
        hrtf8d_process(g_scratch, n, rate_.load(std::memory_order_relaxed));
      }
      const size_t put = ring_->write(g_scratch, (size_t)n * 2);
      const size_t expect = (size_t)n * 2;
      if (put != expect) {
        // 这里永远不该发生（前面已经确认过空间）。真发生了就是环又坏了：计数、记黑匣子，别静默。
        wfail_.fetch_add(1, std::memory_order_relaxed);
        bb_write(YIEYU_LOG_ERROR, "[13] PCM 环写入不全 put=%zu/%zu（环故障）", put, expect);
      } else {
        chunks_.fetch_add(1, std::memory_order_relaxed);
      }
      continue;
    }
    const int eos = (isDsf == 1) ? dsd_eos() : yieyu_pdec_eos();
    if (eos) {
      // 播完：等环里剩下的全部播出去再收工（否则最后 1 秒会被切掉）
      if (ring_->readable() == 0) break;
      sleep_ms(10);
      continue;
    }
    sleep_ms(2);   // 解码器还在要数据
  }

  StopSink();
  if (isDsf == 1) dsd_close();
  else yieyu_pdec_close();
  state_.store(kIdle);
}

void Player::DoSeek(int64_t ms) {
  const int prev = State();
  // 先让音频回调停手（它看到 !kPlaying 就只出静音，不碰环），再动环 → 守住 SPSC
  state_.store(kPreparing);
  sleep_ms(20);
  DrainRing();
  const bool wasDsd = dsd_.load(std::memory_order_relaxed);
  if (wasDsd) {
    if (dsd_seekMs(ms) != 0) LOGE("[16] DSD seek 失败 %lld ms", (long long)ms);
  } else {
    if (yieyu_pdec_seek(ms * 1000) != 0) LOGE("[13] seek 失败 %lld ms", (long long)ms);
    if (resamp_.load(std::memory_order_relaxed)) resample_reset();   // [14] 清历史，别把旧位置的声音带过来
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
