// 总指挥：持有整套状态机与各模块句柄。全进程唯一实例。
// 装载顺序 = docs/DATAFLOW.md §1：验签 → 存储 → 设备 → 解码链 → UI ready
#pragma once
#include <cstddef>
#include <cstdint>
#include <atomic>
#include <memory>

#include "../common/ring.h"

struct Yieyu_Snapshot;  // 全局作用域前向声明（jsb.h 里是具名 struct）

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

  // 播放控制：JSB 只转发到这里，线程模型全在 pipeline/player.cpp
  int  PlayUrl(const char* uri);
  void PauseAudio();
  void ResumeAudio();
  void StopAudio();
  int  SeekMs(int64_t ms);
  void SetVolume(float v);
  void SetDsp(bool on);
  void SetEqGain(int band, float db);
  void SetTone(bool tube, bool vinyl, bool lpf, float drive);

  // 各模块共享的环（唯一的"总线"）
  ByteRing& byteRing() { return *byteRing_; }
  PcmRing&  pcmRing();                       // = 播放器的 PCM 环（唯一样本通道）

 private:
  Engine();
  ~Engine();

  enum class State { kIdle, kLicensing, kUnlicensed, kReady, kPlaying, kFault };
  std::atomic<State> state_{State::kIdle};

  std::unique_ptr<ByteRing> byteRing_;
};

}  // namespace yieyu
