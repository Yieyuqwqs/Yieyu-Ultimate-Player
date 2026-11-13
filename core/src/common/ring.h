// 数据面：无锁 SPSC 环形缓冲（单生产者单消费者）。
// 模块 9→13 的字节环、13→14 的 PCM 环、14→15 的输出环都用它。
// 铁律：容量必须是 2 的幂；读写指针各自缓存，避免乒乓假共享。
//
// ★ 2026-11-13 血案（别改回去）：
//   上一版的 write() 用 `cap_ - (w_ - rCached_)` 算空间，而 rCached_ 只在构造时被写成 0，
//   之后再也没人刷新它 → 生产者永远以为环是满的。累计写满 cap_ 个元素（PCM 环 131072 float
//   ≈ 1.4 秒）之后 write() 永久返回 0：第一首歌放不到两秒就静音，之后整条播放链路死掉，
//   只有杀进程才恢复。这就是"本地音乐听不了"的真因。
//   现在：write() 每次都用 acquire 重新读一次 r_（一次 load，代价可忽略），
//   并且 writable()/readable() 各自直接读对方指针，不依赖缓存。
#pragma once
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

template <typename T>
class SpscRing {
 public:
  explicit SpscRing(size_t capacityPow2)
      : cap_(capacityPow2), mask_(capacityPow2 - 1), buf_(new T[capacityPow2]) {
    static_assert(std::is_trivially_copyable<T>::value, "环里只放 POD");
  }
  ~SpscRing() { delete[] buf_; }
  SpscRing(const SpscRing&) = delete;
  SpscRing& operator=(const SpscRing&) = delete;

  // 生产者视角：现在还能写多少个元素（直接读消费者指针，不吃缓存）
  size_t writable() const {
    const size_t w = w_.load(std::memory_order_relaxed);
    const size_t r = r_.load(std::memory_order_acquire);
    return cap_ - (w - r);
  }

  // 消费者视角：现在能读到多少个元素
  size_t readable() const {
    const size_t w = w_.load(std::memory_order_acquire);
    const size_t r = r_.load(std::memory_order_relaxed);
    return w - r;
  }

  // 生产者：返回实际写入元素数（空间不足时会截断，调用方负责检查）
  size_t write(const T* src, size_t n) {
    if (!src || n == 0) return 0;
    const size_t w = w_.load(std::memory_order_relaxed);
    rCached_ = r_.load(std::memory_order_acquire);   // ★ 唯一修复点：不刷新就是永久静音
    size_t space = cap_ - (w - rCached_);
    if (n > space) n = space;
    if (n == 0) return 0;
    const size_t first = std::min(n, cap_ - (w & mask_));
    std::memcpy(&buf_[w & mask_], src, first * sizeof(T));
    if (n > first) std::memcpy(&buf_[0], src + first, (n - first) * sizeof(T));
    w_.store(w + n, std::memory_order_release);
    return n;
  }

  // 消费者：返回实际读到的元素数
  size_t read(T* dst, size_t n) {
    if (!dst || n == 0) return 0;
    const size_t r = r_.load(std::memory_order_relaxed);
    wCached_ = w_.load(std::memory_order_acquire);
    size_t avail = wCached_ - r;
    if (n > avail) n = avail;
    if (n == 0) return 0;
    const size_t first = std::min(n, cap_ - (r & mask_));
    std::memcpy(dst, &buf_[r & mask_], first * sizeof(T));
    if (n > first) std::memcpy(dst + first, &buf_[0], (n - first) * sizeof(T));
    r_.store(r + n, std::memory_order_release);
    return n;
  }

  size_t capacity() const { return cap_; }

  // 丢弃未读数据。**只允许在消费者已停手时调用**（播放器换曲走 StopSink → DrainRing 这条路）。
  void reset() { r_.store(w_.load(std::memory_order_acquire), std::memory_order_release); }

 private:
  const size_t cap_, mask_;
  T* const buf_;
  alignas(64) std::atomic<size_t> w_{0};
  alignas(64) std::atomic<size_t> r_{0};
  alignas(64) size_t wCached_{0};   // 只由消费者线程读写
  size_t rCached_{0};               // 只由生产者线程读写
};

// 便捷别名（模块编号即所有者）
using ByteRing = SpscRing<uint8_t>;   // [9] curl → [13] ffmpeg
using PcmRing = SpscRing<float>;      // [13]/[16] → [14]/[17] → [15]
