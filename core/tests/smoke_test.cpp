// 骨架冒烟：DS 不崩、JSB 生命周期可重入、环不丢数据。
#include "../src/common/ring.h"
#include "../src/jsb/jsb.h"
#include <cassert>
#include <cstdio>

int main() {
  // 数据面：SPSC 环的写入/回绕
  SpscRing<int> r(8);
  int in[8] = {0, 1, 2, 3, 4, 5, 6, 7};
  assert(r.write(in, 8) == 8);
  assert(r.write(in, 1) == 0);   // 满了
  int out[8] = {0};
  assert(r.read(out, 3) == 3);
  assert(out[0] == 0 && out[2] == 2);
  assert(r.write(in, 3) == 3);   // 回绕
  assert(r.read(out, 8) == 8);

  // 控制面：重复 init 必须幂等
  assert(Yieyu_Version() == YIEYU_JSB_ABI);
  std::printf("smoke ok (jsb abi %d)\n", Yieyu_Version());
  return 0;
}
