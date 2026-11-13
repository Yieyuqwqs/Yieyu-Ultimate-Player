// [8] 插件 ABI —— **我们自己的字节码（YPFX）**，不依赖任何外部格式/引擎。
//
// 设计原则：插件只做"参数级音效"（每采样点算一个增益/滤波系数），所以不需要通用虚拟机：
// 一个极小栈式 VM 足够，指令集只有 10 条，解释器 200 行，bug 面极小。
//
// 文件格式：
//   "YPFX" | version u8 (=1) | kind u8 (0=tremolo) | lfoHz f32 | depth f32 | codeLen u32 | code[]
//
// 状态内存（state[64] 个 f32，宿主在 init 时写入）：
//   state[0] = step = lfoHz / sampleRate
//   state[4] = phase（插件自己维护）
//   state[8] = depth
//
// 指令（1 字节指令码，无操作数；立即数用 PUSHI/PUSHF 先压栈）：
//   0x01 PUSHI <i32 LE>   压 i32（地址用）
//   0x02 PUSHF <f32 LE>   压 f32
//   0x03 LOAD             弹出地址 i32 → 压 state[addr]
//   0x04 STORE            弹地址 i32，再弹值 f32 → state[addr] = value
//   0x05 ADD              a b → a + b       （a 先压、b 后压）
//   0x06 SUB              a b → a - b
//   0x07 MUL              a b → a * b
//   0x08 FLOOR            a → floor(a)
//   0x09 DUP              a → a a
//   0x0A DROP             a → ∅
//   0x0B RET              弹出栈顶 f32 作为返回值（每次调用 = 一个采样点的增益）
//
// 宿主保证：数据类型全 f32（地址除外）；越界地址直接判插件失败并自动关掉插件开关。
#pragma once

namespace yieyu {
namespace plugin {

constexpr char kMagic[4] = {'Y', 'P', 'F', 'X'};
constexpr int  kVersion = 1;
constexpr int  kKindTremolo = 0;
constexpr int  kStateSlots = 64;      // 状态内存槽数（f32）
constexpr int  kMaxCodeBytes = 4096;
constexpr int  kMaxSteps = 4096;      // 一次调用的指令上限（防死循环）

enum Op : unsigned char {
  kOpPushI = 0x01,
  kOpPushF = 0x02,
  kOpLoad = 0x03,
  kOpStore = 0x04,
  kOpAdd = 0x05,
  kOpSub = 0x06,
  kOpMul = 0x07,
  kOpFloor = 0x08,
  kOpDup = 0x09,
  kOpDrop = 0x0A,
  kOpRet = 0x0B,
};

}  // namespace plugin
}  // namespace yieyu
