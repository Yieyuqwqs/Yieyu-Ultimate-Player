// [17] 原生多线程池：只干 DSP 计算，不碰 IO、不碰 UI、不碰磁盘。
// 分块策略：PCM 每块 192 帧（4ms @48k）在 2~4 线程上按"频段/声道"切分，块内无锁。
#pragma once
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace yieyu {

class DspPool {
 public:
  explicit DspPool(int threads) {
    for (int i = 0; i < threads; ++i) workers_.emplace_back([this] { WorkerLoop(); });
  }
  ~DspPool() {
    stop_.store(true);
    { std::lock_guard<std::mutex> lk(mu_); }
    cv_.notify_all();
    for (auto& t : workers_) if (t.joinable()) t.join();
  }

  void Post(std::function<void(int /*workerId*/)> job) {
    { std::lock_guard<std::mutex> lk(mu_); q_.push(std::move(job)); }
    cv_.notify_one();
  }
  int  size() const { return (int)workers_.size(); }

 private:
  void WorkerLoop() {
    for (;;) {
      std::function<void(int)> job;
      {
        std::unique_lock<std::mutex> lk(mu_);
        cv_.wait(lk, [this] { return stop_.load() || !q_.empty(); });
        if (stop_.load() && q_.empty()) return;
        job = std::move(q_.front()); q_.pop();
      }
      job(0);
    }
  }
  std::vector<std::thread> workers_;
  std::queue<std::function<void(int)>> q_;
  std::mutex mu_;
  std::condition_variable cv_;
  std::atomic<bool> stop_{false};
};

}  // namespace yieyu
