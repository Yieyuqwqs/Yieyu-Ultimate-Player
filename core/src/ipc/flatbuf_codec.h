// [21] 跨语言二进制契约：定长 40 字节帧头（零拷贝、逐字节与 idl/frames.fbs 对齐）。
//
// 控制面与数据面共用同一套头：
//   下行命令   magic="YJSC"（= idl/jsb.fbs 的 file_identifier）channels=op  frames=载荷长度
//   上行事件   magic="YFRM"（= idl/frames.fbs 的 file_identifier）channels=事件类型 frames=载荷长度
// 载荷永远紧跟在这 40 字节之后，不许有第二个长度字段（两处长度 = 迟早对不上）。
//
// 改这个布局 = 改契约：`node tools/verify-idl.mjs` 会拿 idl/*.fbs 重新核对一遍，
// 对不上 CI 直接红（模块 21 的"真源校验"）。
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace yieyu {

// 帧头布局（与 fbs 字段顺序一致，共 40 字节）
constexpr int kFrameHeaderBytes = 40;
constexpr int kOffMagic     = 0;   // char[4]
constexpr int kOffVersion   = 4;   // u16
constexpr int kOffChannels  = 6;   // u16
constexpr int kOffSampleRate = 8;  // u32
constexpr int kOffFrames    = 12;  // u32
constexpr int kOffSeq       = 16;  // u64
constexpr int kOffPtsUs     = 24;  // u64
constexpr int kOffChecksum  = 32;  // u64

extern const char kMagicCmd[5];     // "YJSC"
extern const char kMagicFrame[5];   // "YFRM"

// 写头。返回 40；cap 不够返回 -1。
int codec_write(uint8_t* dst, int cap, const char* magic, uint16_t version, uint16_t channels,
                uint32_t sampleRate, uint32_t frames, uint64_t seq, uint64_t ptsUs);

// 校验：0=通过；-1=太短；-2=magic 不符；-3=契约版本不符（ABI 不一致时必须拒绝，不许猜）
int codec_check(const uint8_t* src, int len, const char* magic);

uint16_t codec_u16(const uint8_t* src, int off);
uint32_t codec_u32(const uint8_t* src, int off);
uint64_t codec_u64(const uint8_t* src, int off);
void     codec_put_u32(uint8_t* dst, int off, uint32_t v);
void     codec_put_u64(uint8_t* dst, int off, uint64_t v);

}  // namespace yieyu
