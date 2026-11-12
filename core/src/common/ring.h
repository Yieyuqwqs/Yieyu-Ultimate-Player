// 数据面：无锁 SPSC 环形缓冲（单生产者单消费者）。
// 模块 9→13 的字节环、13→14 的 PCM 环、14→15 的输出环都用它。
// 铁律：容量必须是 2 的幂；读写指针各自缓存，避免乒乓假共享。
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

  size_t writable() const { return cap_ - (w_.load(std::memory_order_relaxed) - rCached_); }
  size_t readable() const { return wCached_ - r_.load(std::memory_order_acquire); }

  // 生产者：返回实际写入元素数（可能少于 n）
  size_t write(const T* src, size_t n) {
    const size_t w = w_.load(std::memory_order_relaxed);
    const size_t space = cap_ - (w - rCached_);
    if (n > space) n = space;
    const size_t first = std::min(n, cap_ - (w & mask_));
    std::memcpy(&buf_[w & mask_], src, first * sizeof(T));
    if (n > first) std::memcpy(&buf_[0], src + first, (n - first) * sizeof(T));
    w_.store(w + n, std::memory_order_release);
    return n;
  }

  // 消费者：返回实际读到的元素数
  size_t read(T* dst, size_t n) {
    const size_t r = r_.load(std::memory_order_relaxed);
    const size_t avail = wCached_ - r;
    if (n > avail) { wCached_ = w_.load(std::memory_order_acquire); if (n > wCached_ - r) n = wCached_ - r; }
    if (n == 0) return 0;
    const size_t first = std::min(n, cap_ - (r & mask_));
    std::memcpy(dst, &buf_[r & mask_], first * sizeof(T));
    if (n > first) std::memcpy(dst + first, &buf_[0], (n - first) * sizeof(T));
    r_.store(r + n, std::memory_order_release);
    return n;
  }

  size_t capacity() const { return cap_; }

 private:
  const size_t cap_, mask_;
  T* const buf_;
  alignas(64) std::atomic<size_t> w_{0};
  alignas(64) std::atomic<size_t> r_{0};
  alignas(64) size_t wCached_{0};
  size_t rCached_{0};
};

// 便捷别名（模块编号即所有者）
using ByteRing = SpscRing<uint8_t>;   // [9] curl → [13] ffmpeg
using PcmRing = SpscRing<float>;      // [13]/[16] → [14]/[17] → [15]
