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
#include <sys/types.h>   /* ssize_t / off64_t */
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdarg.h>

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
static int              g_fd = -1;      /* 本地文件走 fd 打开时持有的句柄 */
static int32_t          g_rate = 0;
static int32_t          g_ch = 2;
static int32_t          g_enc = YIEYU_ENC_PCM16;   /* 输出格式变更后才可信 */
static int              g_inEos = 0;
static int              g_outEos = 0;

/* 最近一次打开失败的原因（给开发者面板看，正常界面不展示） */
static char g_err[200] = {0};

const char* yieyu_pdec_error(void) { return g_err; }

static void set_err(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(g_err, sizeof(g_err), fmt, ap);
  va_end(ap);
}

/* ── 内置 WAV(PCM) 直通：完全不经过 MediaExtractor / MediaCodec ──────────
   为什么必须有它：测试音是 App 自己生成的 WAV。真机实测 setDataSource 对本地
   文件会返回非 OK（rc=-3），如果连"能不能出声"都要靠系统解码器点头，那我们
   永远分不清是"解码坏了"还是"没声音"。这条直通让 [13]→[15] 可以自证。 */
static int      g_wav = 0;              /* 1 = 当前走内置 WAV 直通 */
static uint64_t g_wav_data_off = 0;     /* data 块在文件里的偏移 */
static uint64_t g_wav_data_len = 0;     /* data 块长度（字节） */
static uint64_t g_wav_pos = 0;          /* 已消费字节（相对 data 起点） */
static int      g_wav_ch = 2;
static int      g_wav_rate = 44100;
static int      g_wav_bits = 16;
static int      g_wav_fmt = 1;          /* 1=PCM  3=IEEE float */

static uint32_t rd_u32le(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t rd_u16le(const uint8_t* p) {
  return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

/* 返回 1=是 WAV 且已就绪；0=不是 WAV（交给通用路径）；-1=像 WAV 但读不了 */
static int try_open_wav(int fd, int64_t* durUs) {
  uint8_t hdr[12];
  if (lseek(fd, 0, SEEK_SET) < 0) return 0;
  if (read(fd, hdr, sizeof(hdr)) != (ssize_t)sizeof(hdr)) return 0;
  if (memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) return 0;

  int have_fmt = 0, have_data = 0;
  g_wav_fmt = 1;
  g_wav_ch = 2;
  g_wav_rate = 44100;
  g_wav_bits = 16;
  g_wav_data_off = 0;
  g_wav_data_len = 0;

  uint64_t off = 12;
  for (int guard = 0; guard < 32; ++guard) {
    uint8_t ch[8];
    if (lseek(fd, (off_t)off, SEEK_SET) < 0) break;
    const ssize_t n = read(fd, ch, sizeof(ch));
    if (n != (ssize_t)sizeof(ch)) break;
    const uint32_t csize = rd_u32le(ch + 4);
    if (memcmp(ch, "fmt ", 4) == 0) {
      uint8_t fb[40];
      memset(fb, 0, sizeof(fb));
      size_t take = csize < sizeof(fb) ? (size_t)csize : sizeof(fb);
      if (take < 16) return -1;
      if (read(fd, fb, take) != (ssize_t)take) return -1;
      uint16_t af = rd_u16le(fb);
      if (af == 0xFFFE) af = 1;                  /* WAVE_FORMAT_EXTENSIBLE 当 PCM */
      g_wav_fmt = (int)af;
      g_wav_ch = (int)rd_u16le(fb + 2);
      g_wav_rate = (int)rd_u32le(fb + 4);
      g_wav_bits = (int)rd_u16le(fb + 14);
      have_fmt = 1;
    } else if (memcmp(ch, "data", 4) == 0) {
      g_wav_data_off = off + 8;
      g_wav_data_len = (uint64_t)csize;
      have_data = 1;
    }
    off += 8u + (uint64_t)csize + (uint64_t)(csize & 1u);
    if (have_fmt && have_data) break;
  }

  if (!have_fmt || !have_data || g_wav_data_len < 16) return 0;
  if (g_wav_rate <= 0 || g_wav_ch <= 0 || g_wav_ch > 8) return 0;
  if (g_wav_bits != 8 && g_wav_bits != 16 && g_wav_bits != 24 && g_wav_bits != 32) return 0;

  const uint64_t frame_bytes = (uint64_t)g_wav_ch * (uint64_t)(g_wav_bits / 8);
  if (frame_bytes == 0) return 0;
  const uint64_t frames = g_wav_data_len / frame_bytes;
  if (durUs) *durUs = (int64_t)(frames * 1000000ull / (uint64_t)g_wav_rate);
  g_wav_pos = 0;
  if (lseek(fd, (off_t)g_wav_data_off, SEEK_SET) < 0) return -1;
  g_wav = 1;
  return 1;
}

static int wav_read(float* out, int frames) {
  if (!g_wav || g_fd < 0 || !out || frames <= 0) return 0;
  const int ch = (g_wav_ch > 0) ? g_wav_ch : 2;
  const int bps = g_wav_bits / 8;
  const uint64_t frame_bytes = (uint64_t)ch * (uint64_t)bps;
  if (frame_bytes == 0 || frame_bytes > 16) return 0;

  const uint64_t remain = (g_wav_data_len > g_wav_pos) ? (g_wav_data_len - g_wav_pos) : 0;
  const uint64_t remain_frames = remain / frame_bytes;
  if (remain_frames == 0) return 0;
  int want = frames;
  if ((uint64_t)want > remain_frames) want = (int)remain_frames;

  uint8_t buf[8192];
  int got = 0;
  while (got < want) {
    int chunk = want - got;
    const int cap = (int)(sizeof(buf) / frame_bytes);
    if (chunk > cap) chunk = cap;
    if (chunk <= 0) break;
    const ssize_t n = read(g_fd, buf, (size_t)chunk * (size_t)frame_bytes);
    if (n <= 0) break;
    const int nf = (int)((uint64_t)n / frame_bytes);
    if (nf <= 0) break;
    for (int i = 0; i < nf; ++i) {
      const uint8_t* p = buf + (size_t)i * (size_t)frame_bytes;
      float l = 0.0f, r = 0.0f;
      for (int c = 0; c < 2; ++c) {
        const int src = (c < ch) ? c : 0;
        const uint8_t* q = p + (size_t)src * (size_t)bps;
        float v = 0.0f;
        if (g_wav_fmt == 3 && bps == 4) {
          float f = 0.0f;
          memcpy(&f, q, 4);
          v = f;
        } else if (bps == 1) {
          v = ((float)q[0] - 128.0f) / 128.0f;
        } else if (bps == 2) {
          int16_t s = 0;
          memcpy(&s, q, 2);
          v = (float)s / 32768.0f;
        } else if (bps == 3) {
          int32_t s = (int32_t)((uint32_t)q[0] | ((uint32_t)q[1] << 8) | ((uint32_t)q[2] << 16));
          if (s & 0x800000) s -= 0x1000000;
          v = (float)s / 8388608.0f;
        } else {
          int32_t s = 0;
          memcpy(&s, q, 4);
          v = (float)((double)s / 2147483648.0);
        }
        if (c == 0) l = v; else r = v;
      }
      out[(size_t)(got + i) * 2] = l;
      out[(size_t)(got + i) * 2 + 1] = r;
    }
    got += nf;
    g_wav_pos += (uint64_t)nf * frame_bytes;
  }
  return got;
}



/* 转换暂存：一次输出缓冲可能上万样本，先转到这里，再由 read() 分次拷走 */
#define YIEYU_STAGE_FLOATS 32768
static float   g_stage[YIEYU_STAGE_FLOATS];
static int32_t g_stage_len = 0;
static int32_t g_stage_rd = 0;

static void stage_reset(void) { g_stage_len = 0; g_stage_rd = 0; }

void yieyu_pdec_close(void) {
  if (g_codec) { AMediaCodec_stop(g_codec); AMediaCodec_delete(g_codec); g_codec = NULL; }
  if (g_ex) { AMediaExtractor_delete(g_ex); g_ex = NULL; }
  if (g_fd >= 0) { close(g_fd); g_fd = -1; }   /* fd 方案持有到解码器真正关掉为止 */
  g_wav = 0;
  g_wav_data_off = 0;
  g_wav_data_len = 0;
  g_wav_pos = 0;
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
  g_err[0] = 0;
  yieyu_pdec_close();

  const int is_url = (strncmp(uri, "http://", 7) == 0 || strncmp(uri, "https://", 8) == 0);

  /* ① 本地文件先看是不是 WAV：是的话走内置 PCM 直通，一个系统解码器都不碰。
        测试音必须走这条路 —— 否则"能不能出声"永远取决于系统认不认这个文件。 */
  if (!is_url) {
    const int fd0 = open(uri, O_RDONLY);
    if (fd0 < 0) {
      set_err("open 失败 errno=%d（%s）", errno, uri);
      return -3;
    }
    int64_t wavDur = 0;
    const int wav = try_open_wav(fd0, &wavDur);
    if (wav == 1) {
      g_fd = fd0;
      if (rate) *rate = g_wav_rate;
      if (channels) *channels = g_wav_ch;
      if (durUs) *durUs = wavDur;
      return 0;                      /* ★ 完全绕开 MediaExtractor/MediaCodec */
    }
    close(fd0);
    if (wav < 0) set_err("WAV 头解析失败（文件可能被截断）");
    /* 不是 WAV → 走下面的通用路径 */
  }

  g_ex = AMediaExtractor_new();
  if (!g_ex) { set_err("AMediaExtractor_new 返回空"); return -2; }

  /* ② 通用路径：URL 走系统 MediaHTTP；本地文件先试路径，失败再换 fd。
     ★ fd 兜底：真机实测 setDataSource(绝对路径) 会返回非 OK（rc=-3），
       fd 方案绕开"字符串 → 路径/URI"解析，只要进程能 open 就一定能读。 */
  media_status_t st = AMEDIA_OK;
  if (is_url) {
    st = AMediaExtractor_setDataSource(g_ex, uri);
    if (st != AMEDIA_OK) set_err("setDataSource(URL) 失败 st=%d", (int)st);
  } else {
    st = AMediaExtractor_setDataSource(g_ex, uri);
    if (st != AMEDIA_OK) {
      const int fd = open(uri, O_RDONLY);
      if (fd < 0) {
        set_err("路径打开失败 st=%d，open 也失败 errno=%d", (int)st, errno);
      } else {
        struct stat sst;
        if (fstat(fd, &sst) != 0) {
          set_err("fstat 失败 errno=%d", errno);
          close(fd);
        } else if (sst.st_size <= 0) {
          set_err("文件是空的（%lld 字节）", (long long)sst.st_size);
          close(fd);
        } else {
          const media_status_t st2 =
              AMediaExtractor_setDataSourceFd(g_ex, fd, 0, (off64_t)sst.st_size);
          if (st2 == AMEDIA_OK) {
            g_fd = fd;          /* 交给 close 收尾 */
            st = AMEDIA_OK;
          } else {
            set_err("setDataSourceFd 失败 st=%d（%lld 字节）", (int)st2, (long long)sst.st_size);
            close(fd);
          }
        }
      }
    }
  }

  if (st != AMEDIA_OK) {
    char keep[sizeof(g_err)];
    memcpy(keep, g_err, sizeof(keep));
    LOGE("[13] 打开失败：%s（%.120s）", keep, uri);
    yieyu_pdec_close();
    memcpy(g_err, keep, sizeof(g_err));
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
  if (!out || frames <= 0) return 0;
  if (g_wav) return wav_read(out, frames);      /* 内置 WAV 直通 */
  if (!g_codec) return 0;
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
  if (g_wav) {
    if (g_fd < 0 || g_wav_rate <= 0) return -1;
    if (posUs < 0) posUs = 0;
    const uint64_t frame_bytes = (uint64_t)(g_wav_ch > 0 ? g_wav_ch : 2) *
                                 (uint64_t)(g_wav_bits / 8);
    if (frame_bytes == 0) return -1;
    uint64_t byte = (uint64_t)((double)posUs * (double)g_wav_rate / 1000000.0) * frame_bytes;
    if (byte > g_wav_data_len) byte = g_wav_data_len;
    if (lseek(g_fd, (off_t)(g_wav_data_off + byte), SEEK_SET) < 0) return -2;
    g_wav_pos = byte;
    return 0;
  }
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

int yieyu_pdec_eos(void) {
  if (g_wav) return (g_wav_pos >= g_wav_data_len) ? 1 : 0;
  return g_outEos;
}

#else  /* 非 Android（宿主单测）：给空实现，保证 core 仍可编 */

int yieyu_pdec_open(const char* uri, int32_t* rate, int32_t* channels, int64_t* durUs) {
  (void)uri; (void)rate; (void)channels; (void)durUs;
  return -100;
}
int yieyu_pdec_read(float* out, int frames) { (void)out; (void)frames; return 0; }
int yieyu_pdec_seek(int64_t posUs) { (void)posUs; return -100; }
int yieyu_pdec_eos(void) { return 1; }
void yieyu_pdec_close(void) {}
const char* yieyu_pdec_error(void) { return "宿主平台没有媒体框架"; }

#endif
