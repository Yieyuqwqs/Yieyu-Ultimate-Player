// [21] FlatBuffers：跨语言唯一序列化格式（零拷贝解析）。
// 契约：idl/jsb.fbs（控制面）、idl/frames.fbs（数据面环头）。改契约必须 +1 ABI 版本。
#include "../common/log.h"
#include "../jsb/jsb.h"
#include <cstdint>
#include <cstring>

namespace yieyu {

// 环头（与 idl/frames.fbs 的 FrameHeader 一一对应，48 字节定长，可直接 mmap）
struct FrameHeader {
  uint32_t magic;       // 'Y','F','R','M'
  uint16_t version;     // = YIEYU_JSB_ABI
  uint16_t channels;
  uint32_t sampleRate;
  uint32_t frames;
  uint64_t seq;
  uint64_t ptsUs;
  uint64_t checksum;    // 可选，调试期开
};
static_assert(sizeof(FrameHeader) == 40, "环头必须定长 40 字节");

uint32_t frame_magic() { return 0x5946524Du; /* YFRM */ }

int codec_write_header(uint8_t* dst, int cap, uint32_t sr, uint32_t frames, uint64_t seq) {
  if (!dst || cap < (int)sizeof(FrameHeader)) return -1;
  FrameHeader h{};
  h.magic = frame_magic();
  h.version = YIEYU_JSB_ABI;
  h.channels = 2;
  h.sampleRate = sr;
  h.frames = frames;
  h.seq = seq;
  std::memcpy(dst, &h, sizeof(h));
  return (int)sizeof(h);
}

int codec_check_header(const uint8_t* src, int len) {
  if (!src || len < (int)sizeof(FrameHeader)) return -1;
  FrameHeader h{};
  std::memcpy(&h, src, sizeof(h));
  if (h.magic != frame_magic()) return -2;
  if (h.version != YIEYU_JSB_ABI) { LOGE("[21] 契约版本不符 %u != %d", h.version, YIEYU_JSB_ABI); return -3; }
  return 0;
}

}  // namespace yieyu
