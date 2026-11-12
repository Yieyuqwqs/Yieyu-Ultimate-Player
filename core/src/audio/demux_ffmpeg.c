// [13] FFmpeg：解封装 + 解码（FLAC/ALAC/APE/MP3 → PCM）。
// 不干的事：重采样（[14] JUCE）、DSP（[17]）、网络（[9] curl，用自定义 AVIO 从字节环读）。
#include <stdint.h>
#include <stddef.h>
#include "../common/log.h"

#ifdef YIEYU_OFFLINE
int yieyu_demux_open(const char* path) { (void)path; LOGW("[13]FFmpeg 空实现（YIEYU_OFFLINE）"); return 0; }
int yieyu_demux_read(float* out, int frames) { (void)out; (void)frames; return 0; }
#else
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>

static AVFormatContext* g_fmt;
static AVCodecContext*  g_dec;
static int              g_stream;

/* 关键：AVIO 走模块 9 的字节环，FFmpeg 不自己开 socket */
static int  read_packet(void* opaque, uint8_t* buf, int n) { return yieyu_curl_read(opaque, buf, n); }
static int64_t seek_cb(void* o, int64_t off, int whence)  { return yieyu_curl_seek(o, off, whence); }

int yieyu_demux_open(const char* path) {
  unsigned char* iobuf = av_malloc(32768);
  AVIOContext* avio = avio_alloc_context(iobuf, 32768, 0, NULL, read_packet, NULL, seek_cb);
  g_fmt = avformat_alloc_context();
  g_fmt->pb = avio;
  if (avformat_open_input(&g_fmt, path, NULL, NULL) < 0) { LOGE("[13] open 失败"); return -1; }
  if (avformat_find_stream_info(g_fmt, NULL) < 0) return -2;
  g_stream = av_find_best_stream(g_fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
  const AVCodec* c = avcodec_find_decoder(g_fmt->streams[g_stream]->codecpar->codec_id);
  g_dec = avcodec_alloc_context3(c);
  avcodec_parameters_to_context(g_dec, g_fmt->streams[g_stream]->codecpar);
  avcodec_open2(g_dec, c, NULL);
  LOGI("[13] %s | codec=%s | %d Hz", path, c->name, g_dec->sample_rate);
  return 0;
}

/* 输出统一 float32 平面交错，交给 [14]/[16]/[17]；这里不做任何改变采样率的动作 */
int yieyu_demux_read(float* out, int frames) { (void)out; (void)frames; return 0; /* TODO(P2) */ }
#endif
