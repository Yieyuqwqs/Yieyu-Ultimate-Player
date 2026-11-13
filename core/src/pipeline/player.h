// 播放调度：URI → [13] 解码 → PCM 环 → [15] AAudio。
//
// ★ 线程模型（守铁律 2，谁改谁负责）：
//   · 控制面（Dart / JSB 线程）：只写原子请求，立刻返回，**绝不做网络/文件 IO**；
//   · 解码线程（唯一）：打开（可能阻塞在网络上）→ 解码 → 写 PCM 环；
//   · 音频回调线程：只调 Pull()，纯读原子 + 读环，零锁/零分配/零日志/零 JNI。
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "../common/ring.h"

namespace yieyu {

class Player {
 public:
  static Player& Instance();

  // ── 控制面 ────────────────────────────────────────────────────────
  int  PlayUrl(const char* uri);   // 0=已受理（异步打开）；<0=参数错
  void Pause();
  void Resume();
  void Stop();
  int  SeekMs(int64_t ms);         // 0=已受理；<0=参数错
  void SetVolume(float v);         // 0..1
  void SetDsp(bool on);            // [17] 音效链总开关（默认关）
  bool DspOn() const;
  void SetEqGain(int band, float db);                       // 0..30 段，±24 dB
  void SetTone(bool tube, bool vinyl, bool lpf, float drive);
  void Shutdown();

  // ── 数据面（只在音频线程调用）─────────────────────────────────────
  int  Pull(float* out, int frames);

  // ── 只读状态（UI 每 16ms 拉一次）──────────────────────────────────
  enum { kIdle = 0, kPreparing = 1, kPlaying = 2, kPaused = 3, kError = 4 };
  int     State() const { return state_.load(std::memory_order_relaxed); }
  bool    IsPlaying() const { return State() == kPlaying; }
  int32_t SampleRate() const { return rate_.load(std::memory_order_relaxed); }
  int64_t PositionMs() const;
  int64_t DurationMs() const { return durationMs_.load(std::memory_order_relaxed); }
  float   PeakL() const { return peakL_.load(std::memory_order_relaxed); }
  float   PeakR() const { return peakR_.load(std::memory_order_relaxed); }
  PcmRing& Ring() { return *ring_; }

 private:
  Player();
  ~Player();

  void EnsureThreadLocked();
  void WorkerLoop();
  void OpenAndPlay(const std::string& uri, uint64_t gen);
  void StopSink();
  void DrainRing();
  void DoSeek(int64_t ms);

  std::unique_ptr<PcmRing> ring_;      // 131072 float ≈ 65536 立体声帧，容量恒为 2 的幂
  std::thread              th_;
  std::mutex               mu_;        // 只保护控制面字段，绝不在音频线程碰
  std::condition_variable  cv_;
  std::string              pendingUri_;
  uint64_t                 gen_ = 0;
  bool                     quit_ = false;
  bool                     stopReq_ = false;

  std::atomic<int>     state_{kIdle};
  std::atomic<int32_t> rate_{48000};
  std::atomic<int64_t> baseMs_{0};
  std::atomic<int64_t> playedFrames_{0};
  std::atomic<int64_t> durationMs_{0};
  std::atomic<int64_t> seekReqMs_{-1};
  std::atomic<float>   volume_{1.0f};
  std::atomic<float>   peakL_{0.0f};
  std::atomic<float>   peakR_{0.0f};
  std::atomic<bool>    sinkUp_{false};
  std::atomic<bool>    dspOn_{false};   // [17] 默认旁路
};

}  // namespace yieyu
