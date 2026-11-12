# [12] ARM64 NEON · 常数时间比对

`neon_ct_compare.S` 只有一个函数：`uint64_t yieyu_ct_equal(const uint8_t* a, const uint8_t* b, uint64_t n)`。

## 三条不可谈判的要求

1. **常数时间**：不得有基于数据的早退分支。循环体内全是寄存器运算，差异累积到 `x3` 最后一次性判 0。
2. **防重排**：纯汇编，不经编译器优化。
3. **不等长不泄露**：调用方必须先判长度相等再调用。

## 上机验证（每次改动必跑）

```bash
./build.sh native
adb push out/native/arm64/libyieyu_core.so /data/local/tmp/
adb shell "cd /data/local/tmp && ./ct_leak_test 10000000"   # dudect 风格
```

判定：`|t| < 4.5` 才算过，任何一次超标直接打回。

## 为什么不用 memcmp

- `memcmp` 是 early-exit：时间随"第几位不同"变化 → 计时侧信道可逐步爆破令牌。
- 这条路径在卡密校验热路上（切歌、心跳），要压到微秒级，所以不用外部加密库的封装。
