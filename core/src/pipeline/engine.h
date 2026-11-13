// 总指挥：持有整套状态机与各模块句柄。全进程唯一实例。
// 装载顺序 = docs/DATAFLOW.md §1：验签 → 存储 → 设备 → 解码链 → UI ready
#pragma once
#include <cstddef>
#include <cstdint>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

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
  void PlayerError(char* out, size_t cap) const;

  // ── [5]/[9] 搜歌与点歌（真实网络，走 Go 编的 libyieyu_fetch.so）────────
  // Search: 0=已受理（异步），结果通过 DrainEvents 以 'S'/'E' 事件交给 UI。
  // PlaySong: 0=已受理；内部拼成 yieyu://song?...，直链解析在解码线程做。
  int  Search(const char* keyword);
  int  PlaySong(const char* source, const char* musicId, const char* mid);

  // 各模块共享的环（唯一的"总线"）
  ByteRing& byteRing() { return *byteRing_; }
  PcmRing&  pcmRing();                       // = 播放器的 PCM 环（唯一样本通道）

 private:
  Engine();
  ~Engine();

  void SearchWorker();
  void PushEvent(char type, const std::string& payload);

  enum class State { kIdle, kLicensing, kUnlicensed, kReady, kPlaying, kFault };
  std::atomic<State> state_{State::kIdle};
  // [10] 验签库状态：-1=没有库/还没跑，0=库在但内嵌公钥缺失，1=可用
  std::atomic<int> lic_{-1};

  std::unique_ptr<ByteRing> byteRing_;

  // 搜歌线程（只有一条，串行化请求，避免用户连点把接口打爆）
  // ★ 起来后立刻 detach：进程退出时不会因为 joinable 触发 std::terminate。
  std::thread              searchTh_;
  bool                     searchStarted_ = false;
  std::mutex               searchMu_;
  std::condition_variable  searchCv_;
  std::string              searchKw_;
  bool                     searchPending_ = false;
  bool                     searchQuit_ = false;

  // 事件槽（单槽：UI 每 16ms 拉一次，够用且零分配）
  std::mutex               evtMu_;
  std::string              evtPayload_;
  char                     evtType_ = 0;
};

}  // namespace yieyu
