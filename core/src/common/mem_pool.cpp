// [4] 定长内存池（零依赖：不编 Zig，也不引任何分配器）
//
// 用途单一：JSB 的命令帧（≤4KB）从这里拿块，避免每条 UI 命令都走 malloc。
// 实现：8 档固定尺寸（64/128/256/512/1024/2048/4096）单链表空闲表 + 一个 mmap 大块。
// 线程安全：单把互斥锁（调用频率是"每条 UI 命令一次"，不在音频线程）。
#include <stddef.h>
#include <stdint.h>
#include <sys/mman.h>
#include <mutex>
#include <cstring>

namespace {
constexpr int kClasses = 7;
constexpr size_t kSizes[kClasses] = {64, 128, 256, 512, 1024, 2048, 4096};

struct Block {
  Block* next;
  int cls;
  size_t usable;
};

Block* g_free[kClasses] = {nullptr};
std::mutex g_mu;
uint8_t* g_pool = nullptr;
size_t g_poolBytes = 0;
size_t g_used = 0;

int classOf(size_t n) {
  for (int i = 0; i < kClasses; ++i) if (n <= kSizes[i]) return i;
  return -1;
}
}  // namespace

extern "C" {

int yieyu_mem_init(unsigned int blocksPerClass) {
  std::lock_guard<std::mutex> lk(g_mu);
  if (g_pool) return 0;
  if (blocksPerClass == 0 || blocksPerClass > 4096) blocksPerClass = 512;
  size_t need = 0;
  for (int i = 0; i < kClasses; ++i) need += (size_t)blocksPerClass * (sizeof(Block) + kSizes[i] + 16);
  size_t pages = (need + 4095) / 4096 * 4096;
  void* p = mmap(nullptr, pages, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) return -1;
  g_pool = (uint8_t*)p;
  g_poolBytes = pages;
  size_t off = 0;
  for (int i = 0; i < kClasses; ++i) {
    for (unsigned int b = 0; b < blocksPerClass; ++b) {
      Block* blk = (Block*)(g_pool + off);
      off += sizeof(Block) + kSizes[i] + 16;
      if (off > pages) break;
      blk->cls = i;
      blk->usable = kSizes[i];
      blk->next = g_free[i];
      g_free[i] = blk;
    }
  }
  g_used = 0;
  return 0;
}

void* yieyu_mem_alloc(size_t size) {
  const int cls = classOf(size);
  if (cls < 0) return nullptr;
  std::lock_guard<std::mutex> lk(g_mu);
  Block* b = g_free[cls];
  if (!b) return nullptr;
  g_free[cls] = b->next;
  ++g_used;
  return (void*)((uint8_t*)b + sizeof(Block));
}

void yieyu_mem_free(void* p, size_t size) {
  if (!p) return;
  const int cls = classOf(size);
  if (cls < 0) return;
  Block* b = (Block*)((uint8_t*)p - sizeof(Block));
  if (b->cls != cls) return;   // 尺寸对不上：宁可泄漏也不破坏池子
  std::lock_guard<std::mutex> lk(g_mu);
  b->next = g_free[cls];
  g_free[cls] = b;
  if (g_used) --g_used;
}

int yieyu_mem_alive(void) {
  std::lock_guard<std::mutex> lk(g_mu);
  return g_pool ? 1 : 0;
}

size_t yieyu_mem_used(void) {
  std::lock_guard<std::mutex> lk(g_mu);
  return g_used;
}

}  // extern "C"

# 引擎/JSB 里用到 mem_pool 与 license/store，宏在 CMake 里统一定义
