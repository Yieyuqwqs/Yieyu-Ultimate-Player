// 总指挥：持有整套状态机与各模块句柄。全进程唯一实例。
// 装载顺序 = docs/DATAFLOW.md §1：验签 → 存储 → 设备 → 解码链 → UI ready
#pragma once
#include <cstddef>
#include <cstdint>
#include <atomic>
#include <memory>

#include "../common/ring.h"

struct Yieyu_Snapshot;  // jsb.h

namespace yieyu {

class Engine {
 public:
  static Engine& Instance();

  int  Boot(const char* filesDir, const char* abi);
  void Shutdown();

  int    HandleCommand(const uint8_t* fbs, size_t len);  // 返回 0=ok, <0=错误码
  size_t DrainEvents(uint8_t* out, size_t cap);
  int    Snapshot(Yieyu_Snapshot* out);
  void   OnSystemAction(int actionId);
  void   Stats(char* out, size_t cap);

  // 各模块共享的环（唯一的"总线"）
  ByteRing& byteRing() { return *byteRing_; }
  PcmRing&  pcmRing()  { return *pcmRing_; }

 private:
  Engine();
  ~Engine();

  enum class State { kIdle, kLicensing, kUnlicensed, kReady, kPlaying, kFault };
  std::atomic<State> state_{State::kIdle};

  std::unique_ptr<ByteRing> byteRing_;
  std::unique_ptr<PcmRing>  pcmRing_;
  std::atomic<int>  sampleRate_{48000};
  std::atomic<int64_t> positionMs_{0}, durationMs_{0};
  std::atomic<float> peakL_{0}, peakR_{0};
};

}  // namespace yieyu
