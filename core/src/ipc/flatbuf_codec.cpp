#include "flatbuf_codec.h"
#include <cstring>
#include "../common/crc32.h"
#include "../common/log.h"
#include "../jsb/jsb.h"

namespace yieyu {

const char kMagicCmd[5]   = {'Y', 'J', 'S', 'C', '\0'};
const char kMagicFrame[5] = {'Y', 'F', 'R', 'M', '\0'};

int codec_write(uint8_t* dst, int cap, const char* magic, uint16_t version, uint16_t channels,
                uint32_t sampleRate, uint32_t frames, uint64_t seq, uint64_t ptsUs) {
  if (!dst || !magic || cap < kFrameHeaderBytes) return -1;
  std::memset(dst, 0, (size_t)kFrameHeaderBytes);
  std::memcpy(dst + kOffMagic, magic, 4);
  std::memcpy(dst + kOffVersion, &version, 2);
  std::memcpy(dst + kOffChannels, &channels, 2);
  std::memcpy(dst + kOffSampleRate, &sampleRate, 4);
  std::memcpy(dst + kOffFrames, &frames, 4);
  std::memcpy(dst + kOffSeq, &seq, 8);
  std::memcpy(dst + kOffPtsUs, &ptsUs, 8);
  return kFrameHeaderBytes;
}

int codec_check(const uint8_t* src, int len, const char* magic) {
  if (!src || !magic || len < kFrameHeaderBytes) return -1;
  if (std::memcmp(src + kOffMagic, magic, 4) != 0) return -2;
  if (codec_u16(src, kOffVersion) != YIEYU_JSB_ABI) {
    LOGE("[21] 契约版本不符：帧=%u 本机=%d", (unsigned)codec_u16(src, kOffVersion), YIEYU_JSB_ABI);
    return -3;
  }
  return 0;
}

uint16_t codec_u16(const uint8_t* src, int off) {
  uint16_t v = 0;
  std::memcpy(&v, src + off, 2);
  return v;
}
uint32_t codec_u32(const uint8_t* src, int off) {
  uint32_t v = 0;
  std::memcpy(&v, src + off, 4);
  return v;
}
uint64_t codec_u64(const uint8_t* src, int off) {
  uint64_t v = 0;
  std::memcpy(&v, src + off, 8);
  return v;
}
void codec_put_u32(uint8_t* dst, int off, uint32_t v) { std::memcpy(dst + off, &v, 4); }
void codec_put_u64(uint8_t* dst, int off, uint64_t v) { std::memcpy(dst + off, &v, 8); }

}  // namespace yieyu
