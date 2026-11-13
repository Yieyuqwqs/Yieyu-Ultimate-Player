// [13] 平台解码后端实现：AMediaExtractor（解封装）+ AMediaCodec（解码）。
//
// 线程约定：本文件的函数**全部**只能由解码线程（player.cpp 的 worker）调用；
//           它不持有任何锁、不起线程、不写日志（只有出错才写黑匣子）。
// 数据约定：输出一律 float32 立体声交错（mono 复制成两路，>2 声道只取前两路）。
#include "demux_platform.h"

#if defined(__ANDROID__)

#include <media/NdkMediaCodec.h>
#include <media/NdkMediaExtractor.h>
#include <media/NdkMediaFormat.h>

#include <stddef.h>
#include <string.h>
#include <sys/types.h>   /* ssize_t */

#include "../common/log.h"

#define YIEYU_ENC_PCM16  2  /* AMEDIAFORMAT_PCM_16BIT */
#define YIEYU_ENC_FLOAT  4  /* AMEDIAFORMAT_PCM_FLOAT */

/* ★ NDK 的 AMEDIAFORMAT_KEY_PCM_ENCODING 到 API 28 才进头文件（带 __INTRODUCED_IN(28)），
   而本工程 minSdk = 26（AAudio 的门槛）→ 直接引用会编译失败。
   这个 key 的字符串值自 API 21 起就稳定是 "pcm-encoding"，写字面量即可。 */
#define YIEYU_KEY_PCM_ENCODING "pcm-encoding"


/* ── 状态（单实例，单线程使用）───────────────────────────────────────── */
static AMediaExtractor* g_ex = NULL;
static AMediaCodec*     g_codec = NULL;
static int32_t          g_rate = 0;
static int32_t          g_ch = 2;
static int32_t          g_enc = YIEYU_ENC_PCM16;   /* 输出格式变更后才可信 */
static int              g_inEos = 0;
static int              g_outEos = 0;

/* 转换暂存：一次输出缓冲可能上万样本，先转到这里，再由 read() 分次拷走 */
#define YIEYU_STAGE_FLOATS 32768
static float   g_stage[YIEYU_STAGE_FLOATS];
static int32_t g_stage_len = 0;
static int32_t g_stage_rd = 0;

static void stage_reset(void) { g_stage_len = 0; g_stage_rd = 0; }

void yieyu_pdec_close(void) {
  if (g_codec) { AMediaCodec_stop(g_codec); AMediaCodec_delete(g_codec); g_codec = NULL; }
  if (g_ex) { AMediaExtractor_delete(g_ex); g_ex = NULL; }
  g_inEos = g_outEos = 0;
  stage_reset();
  g_rate = 0;
  g_ch = 2;
  g_enc = YIEYU_ENC_PCM16;
}

/* 把一块解码输出转成 float 立体声，写进 g_stage */
static void convert_block(const uint8_t* src, size_t bytes) {
  const int ch = (g_ch > 0) ? g_ch : 2;
  const int is_float = (g_enc == YIEYU_ENC_FLOAT);
  const size_t bps = is_float ? 4u : 2u;
  const size_t frame_bytes = bps * (size_t)ch;
  if (frame_bytes == 0) return;
  size_t frames = bytes / frame_bytes;
  size_t out = 0;
  for (size_t i = 0; i < frames; ++i) {
    if (out + 2 > (size_t)YIEYU_STAGE_FLOATS) break;
    const uint8_t* p = src + i * frame_bytes;
    float left, right;
    if (is_float) {
      float l = 0.0f, r = 0.0f;
      memcpy(&l, p, 4);
      if (ch > 1) memcpy(&r, p + 4, 4); else r = l;
      left = l; right = r;
    } else {
      int16_t l = 0, r = 0;
      memcpy(&l, p, 2);
      if (ch > 1) memcpy(&r, p + 2, 2); else r = l;
      left = (float)l / 32768.0f;
      right = (float)r / 32768.0f;
    }
    g_stage[out++] = left;
    g_stage[out++] = right;
  }
  g_stage_len = (int32_t)out;
  g_stage_rd = 0;
}

/* 读一次输出格式，更新 PCM 编码/声道（解码器常在收到第一帧后才给出真格式）*/
static void refresh_output_format(void) {
  AMediaFormat* of = AMediaCodec_getOutputFormat(g_codec);
  if (!of) return;
  int32_t enc = 0, ch = 0;
  if (AMediaFormat_getInt32(of, YIEYU_KEY_PCM_ENCODING, &enc) && enc > 0) g_enc = enc;
  if (AMediaFormat_getInt32(of, AMEDIAFORMAT_KEY_CHANNEL_COUNT, &ch) && ch > 0) g_ch = ch;
  AMediaFormat_delete(of);
}

/* 推进一格编解码管线；返回 1=这次拿到了新样本 */
static int pump_once(void) {
  AMediaCodecBufferInfo info;
  memset(&info, 0, sizeof(info));
  ssize_t oi = AMediaCodec_dequeueOutputBuffer(g_codec, &info, 0);
  if (oi >= 0) {
    size_t cap = 0;
    uint8_t* buf = AMediaCodec_getOutputBuffer(g_codec, (size_t)oi, &cap);
    if (buf && info.size > 0) convert_block(buf + info.offset, (size_t)info.size);
    AMediaCodec_releaseOutputBuffer(g_codec, (size_t)oi, false);
    if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) g_outEos = 1;
    return g_stage_len > 0 ? 1 : 0;
  }
  if (oi == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) { refresh_output_format(); return 0; }
  if (oi == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) { return 0; }

  /* 没有可取的输出 → 优先喂输入（快，别等）*/
  if (!g_inEos) {
    ssize_t ii = AMediaCodec_dequeueInputBuffer(g_codec, 0);
    if (ii >= 0) {
      size_t icap = 0;
      uint8_t* ib = AMediaCodec_getInputBuffer(g_codec, (size_t)ii, &icap);
      if (!ib || icap == 0) {
        AMediaCodec_queueInputBuffer(g_codec, (size_t)ii, 0, 0, 0, 0);
        return 0;
      }
      ssize_t n = AMediaExtractor_readSampleData(g_ex, ib, icap);
      if (n < 0) {
        AMediaCodec_queueInputBuffer(g_codec, (size_t)ii, 0, 0, 0,
                                     AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
        g_inEos = 1;
      } else {
        int64_t pts = AMediaExtractor_getSampleTime(g_ex);
        AMediaCodec_queueInputBuffer(g_codec, (size_t)ii, 0, (size_t)n, pts, 0);
        AMediaExtractor_advance(g_ex);
      }
    }
  }
  return 0;
}

int yieyu_pdec_open(const char* uri, int32_t* rate, int32_t* channels, int64_t* durUs) {
  if (!uri || !*uri) return -1;
  yieyu_pdec_close();

  g_ex = AMediaExtractor_new();
  if (!g_ex) return -2;

  /* 默认超时 0 = 一直等到拿到数据；http(s) 由系统 MediaHTTP 负责 */
  if (AMediaExtractor_setDataSource(g_ex, uri) != AMEDIA_OK) {
    LOGE("[13] 打开失败（setDataSource）: %.200s", uri);
    yieyu_pdec_close();
    return -3;
  }

  const size_t ntracks = AMediaExtractor_getTrackCount(g_ex);
  ssize_t track = -1;
  for (size_t i = 0; i < ntracks; ++i) {
    AMediaFormat* f = AMediaExtractor_getTrackFormat(g_ex, i);
    if (!f) continue;
    const char* mime = NULL;
    AMediaFormat_getString(f, AMEDIAFORMAT_KEY_MIME, &mime);
    if (mime && strncmp(mime, "audio/", 6) == 0) { track = (ssize_t)i; AMediaFormat_delete(f); break; }
    AMediaFormat_delete(f);
  }
  if (track < 0) {
    LOGE("[13] 文件里没有音轨: %.160s", uri);
    yieyu_pdec_close();
    return -4;
  }

  AMediaFormat* fmt = AMediaExtractor_getTrackFormat(g_ex, track);
  if (!fmt) { yieyu_pdec_close(); return -5; }
  int32_t sr = 0, ch = 0;
  int64_t dur = 0;
  AMediaFormat_getInt32(fmt, AMEDIAFORMAT_KEY_SAMPLE_RATE, &sr);
  AMediaFormat_getInt32(fmt, AMEDIAFORMAT_KEY_CHANNEL_COUNT, &ch);
  AMediaFormat_getInt64(fmt, AMEDIAFORMAT_KEY_DURATION, &dur);
  const char* mime = NULL;
  AMediaFormat_getString(fmt, AMEDIAFORMAT_KEY_MIME, &mime);
  // ★ mime 指向 fmt 内部内存，fmt 一 delete 就悬空 → 先拷到栈上再用
  char mimebuf[64];
  memset(mimebuf, 0, sizeof(mimebuf));
  strncpy(mimebuf, mime ? mime : "audio/mp4a-latm", sizeof(mimebuf) - 1);
  mime = mimebuf;

  g_codec = AMediaCodec_createDecoderByType(mime);
  if (!g_codec) {
    LOGE("[13] 建解码器失败 mime=%s", mime);
    AMediaFormat_delete(fmt);
    yieyu_pdec_close();
    return -6;
  }
  const media_status_t cs = AMediaCodec_configure(g_codec, fmt, NULL, NULL, 0);
  AMediaFormat_delete(fmt);
  if (cs != AMEDIA_OK) {
    LOGE("[13] 解码器配置失败 st=%d（mime=%s）", (int)cs, mime);
    yieyu_pdec_close();
    return -7;
  }
  AMediaExtractor_selectTrack(g_ex, track);
  if (AMediaCodec_start(g_codec) != AMEDIA_OK) {
    LOGE("[13] 解码器启动失败");
    yieyu_pdec_close();
    return -8;
  }

  g_rate = (sr > 0) ? sr : 44100;
  g_ch = (ch > 0) ? ch : 2;
  g_enc = YIEYU_ENC_PCM16;
  g_inEos = g_outEos = 0;
  stage_reset();

  if (rate) *rate = g_rate;
  if (channels) *channels = g_ch;
  if (durUs) *durUs = dur;
  return 0;
}

int yieyu_pdec_read(float* out, int frames) {
  if (!out || frames <= 0 || !g_codec) return 0;
  int produced = 0;               /* 已产出帧数 */
  int guard = 24;                 /* 每次调用最多推进 24 次，避免在解码线程里空转 */
  while (produced < frames && guard-- > 0) {
    /* ① 先把暂存里剩的搬走 */
    if (g_stage_rd < g_stage_len) {
      const int32_t avail = (g_stage_len - g_stage_rd) / 2;   /* 帧 */
      int32_t take = frames - produced;
      if (take > avail) take = avail;
      memcpy(out + (size_t)produced * 2, g_stage + g_stage_rd, (size_t)take * 2 * sizeof(float));
      g_stage_rd += take * 2;
      produced += take;
      if (g_stage_rd >= g_stage_len) stage_reset();
      continue;
    }
    /* ② 暂存空了 → 推进一格 */
    if (!pump_once()) {
      if (g_outEos) break;
      /* 暂时没数据（输入还没喂够）；交给上层稍后再来 */
      break;
    }
  }
  return produced;
}

int yieyu_pdec_seek(int64_t posUs) {
  if (!g_ex || !g_codec) return -1;
  if (posUs < 0) posUs = 0;
  if (AMediaExtractor_seekTo(g_ex, posUs, AMEDIAEXTRACTOR_SEEK_CLOSEST_SYNC) != AMEDIA_OK) {
    LOGE("[13] seek 失败 pos=%lld", (long long)posUs);
    return -2;
  }
  AMediaCodec_flush(g_codec);
  g_inEos = g_outEos = 0;
  stage_reset();
  return 0;
}

int yieyu_pdec_eos(void) { return g_outEos; }

#else  /* 非 Android（宿主单测）：给空实现，保证 core 仍可编 */

int yieyu_pdec_open(const char* uri, int32_t* rate, int32_t* channels, int64_t* durUs) {
  (void)uri; (void)rate; (void)channels; (void)durUs;
  return -100;
}
int yieyu_pdec_read(float* out, int frames) { (void)out; (void)frames; return 0; }
int yieyu_pdec_seek(int64_t posUs) { (void)posUs; return -100; }
int yieyu_pdec_eos(void) { return 1; }
void yieyu_pdec_close(void) {}

#endif
