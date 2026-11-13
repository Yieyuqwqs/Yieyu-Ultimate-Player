// 总指挥。真正的"业务决策"只有三件：能不能播、播什么、出错怎么办。
#include "engine.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "../jsb/jsb.h"
#include "../common/log.h"
#include "../common/blackbox.h"
#include "../common/app_paths.h"
#include "../common/integrity.h"
#include "../dsp/dsp.h"
#include "../ipc/flatbuf_codec.h"
#include "../sensor/gyro_asensor.h"
#include "../store/kv_store.h"
#include "../audio/aaudio_sink.h"
#include "../script/lua_sandbox.h"
#include "../ai/vocals.h"
#include "../plugin/plugin_host.h"
#include "player.h"

// [5]/[9] 搜歌出口：Go 编的 libyieyu_fetch.so（与 Lua 的 http_* 是同一份）
#ifdef YIEYU_HAVE_FETCH
extern "C" {
char* yieyu_music_search(const char* keyword, int page, int limit, int* outLen);
void  yieyu_fetch_free(char* p);
}
#endif

// [10] 验签 / [19] 只读库：Rust 编的 cdylib，运行时 dlopen（不做静态链：
// cdylib 没有 SONAME，链进 core 会把绝对路径写进 DT_NEEDED，装机 dlopen 必炸）。
#if defined(YIEYU_HAVE_LICENSE) || defined(YIEYU_HAVE_STORE)
#include <dlfcn.h>
#endif

namespace yieyu {

Engine& Engine::Instance() {
  static Engine e;
  return e;
}

Engine::Engine()
    : byteRing_(new ByteRing(1u << 18)) {}   // 256 KiB 字节环（[9] 拉流用，3s 预取）

Engine::~Engine() = default;

PcmRing& Engine::pcmRing() { return Player::Instance().Ring(); }

int Engine::Boot(const char* filesDir, const char* abi) {
  // ⓪ 路径先定死（唯一真相），再起黑匣子：之后任何模块要写盘都问 app_paths
  const int pub = yp_init(filesDir);
  if (bb_init(yp_home()) != 0) {
    // 连私有目录都写不进：没有日志可落，但 App 仍要起来
  }
  if (!pub) {
    bb_write(YIEYU_LOG_ERROR, "[0] 公共目录不可写，运行目录落在私有目录：%s", yp_home());
  }

  state_.store(State::kLicensing);

  // 允许 "Shutdown → 再 Init"（面板热重载会走这条路）：搜歌线程状态复位
  {
    std::lock_guard<std::mutex> lk(searchMu_);
    searchStarted_ = false;
    searchQuit_ = false;
    searchPending_ = false;
    searchKw_.clear();
  }

  // ① [10] 验签：唯一密钥在这里，Kotlin/Dart/Lua 一律不持有。
  bool licensed = true;
#ifdef YIEYU_HAVE_LICENSE
  {
    void* h = dlopen("libyieyu_license.so", RTLD_NOW | RTLD_LOCAL);
    if (!h) {
      const char* e = dlerror();
      lic_.store(-1);
      bb_write(YIEYU_LOG_ERROR, "[10] dlopen libyieyu_license.so 失败：%s", e ? e : "?");
    } else {
      typedef int (*has_pubkey_fn)(void);
      has_pubkey_fn fn = nullptr;
      *reinterpret_cast<void**>(&fn) = dlsym(h, "yieyu_license_has_pubkey");
      if (!fn) {
        lic_.store(-1);
        bb_write(YIEYU_LOG_ERROR, "[10] 验签库里没有 yieyu_license_has_pubkey 符号");
      } else {
        const int hasKey = fn();
        lic_.store(hasKey == 1 ? 1 : 0);
        if (hasKey != 1) {
          bb_write(YIEYU_LOG_ERROR, "[10] 验签库在，但内嵌公钥缺失（has_pubkey=%d）", hasKey);
        }
      }
      // ★ 故意不 dlclose：验签句柄要活到进程结束
    }
  }
#else
  lic_.store(-1);
  bb_write(YIEYU_LOG_ERROR, "[10] 构建期就没有 libyieyu_license.so，验签走空实现");
#endif
  if (!licensed) {
    state_.store(State::kUnlicensed);
    bb_write(YIEYU_LOG_ERROR, "[10] 验签失败 → 熔断，trace=%s", bb_trace());
    return 0;
  }

  // ② [20] 打开本地存储（唯一写者），并恢复上次会话
  if (kv_open(yp_data()) != 0) {
    state_.store(State::kFault);
    bb_write(YIEYU_LOG_FATAL, "[20] 存储打不开 dir=%s（%s）", yp_data(), kv_last_error());
    return -2;
  }
  {
    char buf[512];
    int n = kv_get("vol", buf, (int)sizeof(buf) - 1);
    if (n > 0) {
      buf[n] = 0;
      const float v = (float)std::strtod(buf, nullptr);
      if (v >= 0.0f && v <= 1.0f) { vol_.store(v); Player::Instance().SetVolume(v); }
    }
    n = kv_get("dsp", buf, (int)sizeof(buf) - 1);
    if (n > 0) {
      buf[n] = 0;
      const int on = std::atoi(buf);
      dspOn_.store(on ? 1 : 0);
      Player::Instance().SetDsp(on != 0);
    }
    n = kv_get("last_uri", buf, (int)sizeof(buf) - 1);
    if (n > 0) { buf[n] = 0; lastUri_.assign(buf, (size_t)n); }
  }

  // ③ [19] 只读库：可选；缺库只是少一个诊断视角，不许影响启动
#ifdef YIEYU_HAVE_STORE
  {
    storeLib_ = dlopen("libyieyu_store.so", RTLD_NOW | RTLD_LOCAL);
    if (storeLib_) {
      typedef int (*open_fn)(const char*);
      open_fn op = nullptr;
      *reinterpret_cast<void**>(&op) = dlsym(storeLib_, "yieyu_store_open");
      if (op && kv_path() && *kv_path()) {
        const int rc = op(kv_path());
        if (rc != 0) bb_write(YIEYU_LOG_ERROR, "[19] 只读库打不开 %s rc=%d", kv_path(), rc);
        typedef int (*count_fn)(void);
        count_fn cf = nullptr;
        *reinterpret_cast<void**>(&cf) = dlsym(storeLib_, "yieyu_store_count");
        storeEntries_.store(cf ? cf() : -1);
      }
    } else {
      const char* e = dlerror();
      bb_write(YIEYU_LOG_ERROR, "[19] dlopen libyieyu_store.so 失败：%s", e ? e : "?");
      storeLib_ = nullptr;
    }
  }
#else
  storeEntries_.store(-1);
#endif

  // ③.5 [8] wasm 插件：<home>/plugins/*.wasm（没有插件是正常情况，不算错）
  {
    char pdir[900];
    std::snprintf(pdir, sizeof(pdir), "%s/plugins", yp_home());
    (void)yp_ensure(pdir);
    if (plugin_host_init(pdir) != 0) {
      // 没有插件是常态：只在真有文件却载入失败时才记黑盒
      if (plugin_count() > 0) {
        bb_write(YIEYU_LOG_ERROR, "[8] 插件载入失败：%s（目录 %s）", plugin_error(), pdir);
      }
    }
  }

  // ④ [23] 陀螺仪（8D 用；没有传感器就静默降级，不外抛）
  gyro_start(200 /* Hz */);

  // ⑤ [7] Lua 沙箱：必须在这里点火（启动时再扫一遍脚本目录，重启后不用重新导入）
  if (lua_sandbox_init(yp_scripts()) != 0) {
    bb_write(YIEYU_LOG_ERROR, "[7] Lua 沙箱初始化失败（脚本将不可用）dir=%s", yp_scripts());
  } else {
    int loaded = 0, rejected = 0;
    lua_sandbox_reload(yp_scripts(), &loaded, &rejected);
    if (loaded == 0 && rejected > 0) {
      bb_write(YIEYU_LOG_ERROR, "[7] 启动扫描：登记 0 个、被拒 %d 个（目录 %s）", rejected, yp_scripts());
    }
  }

  // ⑥ [11] 自校验：只记录结果（期望摘要在 APK 资产里，由 M1 调 Yieyu_IntegrityCheck 传进来）
  (void)yieyu_self_digest(selfDigest_);

  state_.store(State::kReady);
  return 0;
}

void Engine::SaveSession() {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.6f", (double)vol_.load());
  kv_put("vol", buf, (int)std::strlen(buf));
  const int on = dspOn_.load() ? 1 : 0;
  kv_put("dsp", on ? "1" : "0", 1);
  if (!lastUri_.empty()) kv_put("last_uri", lastUri_.data(), (int)lastUri_.size());
  kv_flush();
}

void Engine::Shutdown() {
  // 搜歌线程先停：它可能正阻塞在网络上，只置标志 + 唤醒，join 会卡住退出。
  {
    std::lock_guard<std::mutex> lk(searchMu_);
    searchQuit_ = true;
    searchPending_ = false;
  }
  searchCv_.notify_all();

  Player::Instance().Shutdown();
  gyro_stop();
  SaveSession();                       // [20] 会话落盘（音量 / 音效开关 / 上次曲目）
#ifdef YIEYU_HAVE_STORE
  if (storeLib_) {
    typedef void (*close_fn)(void);
    close_fn cf = nullptr;
    *reinterpret_cast<void**>(&cf) = dlsym(storeLib_, "yieyu_store_close");
    if (cf) cf();
    dlclose(storeLib_);
    storeLib_ = nullptr;
    storeEntries_.store(-1);
  }
#endif
  kv_close();
  lua_sandbox_shutdown();
  plugin_shutdown();
  bb_flush();
  bb_shutdown();
  state_.store(State::kIdle);
}

int Engine::PlayUrl(const char* uri) {
  if (uri && *uri) {
    std::lock_guard<std::mutex> lk(evtMu_);
    lastUri_.assign(uri);              // 只缓存，落盘在 SaveSession（避免拖动进度就写盘）
  }
  return Player::Instance().PlayUrl(uri);
}
void Engine::PauseAudio() { Player::Instance().Pause(); }
void Engine::ResumeAudio() { Player::Instance().Resume(); }
void Engine::StopAudio() { Player::Instance().Stop(); }
int  Engine::SeekMs(int64_t ms) { return Player::Instance().SeekMs(ms); }
void Engine::SetVolume(float v) { vol_.store(v); Player::Instance().SetVolume(v); }
void Engine::PlayerError(char* out, size_t cap) const { Player::Instance().LastError(out, cap); }
void Engine::SetDsp(bool on) { dspOn_.store(on ? 1 : 0); Player::Instance().SetDsp(on); }
void Engine::SetEqGain(int band, float db) { Player::Instance().SetEqGain(band, db); }
void Engine::SetRate(int hz) { Player::Instance().SetTargetRate(hz); }
void Engine::SetPlugin(bool on) { Player::Instance().SetPlugin(on); }

// ── [18] 离线人声消除（只在后台线程跑，绝不进音频线程）────────────────
int Engine::KaraokeStart(const char* srcWav, const char* dstWav) {
  if (!srcWav || !dstWav || !*srcWav || !*dstWav) return -1;
  int expect = -2;
  if (!karaoke_.compare_exchange_strong(expect, -1)) return -2;   // 已经在跑
  {
    std::lock_guard<std::mutex> lk(karaokeMu_);
    karaokeOut_.assign(dstWav);
  }
  const std::string src(srcWav), dst(dstWav);
  std::thread([this, src, dst] {
    const int rc = yieyu_vocals_render(src.c_str(), dst.c_str(), nullptr);
    karaoke_.store(rc == 0 ? 0 : (rc < 0 ? rc : -3));
    if (rc != 0) {
      bb_write(YIEYU_LOG_ERROR, "[18] 人声消除失败 rc=%d（%s）", rc, yieyu_vocals_error());
    }
  }).detach();
  return 0;
}

int Engine::KaraokeStatus(char* out, size_t cap) const {
  const int st = karaoke_.load();
  if (out && cap > 0) {
    std::lock_guard<std::mutex> lk(karaokeMu_);
    std::snprintf(out, cap, "%s", st == 0 ? karaokeOut_.c_str() : (st < -1 ? yieyu_vocals_error() : ""));
  }
  return st;
}
void Engine::SetTone(bool tube, bool vinyl, bool lpf, float drive) {
  Player::Instance().SetTone(tube, vinyl, lpf, drive);
}

// ── [11] 防篡改 ──────────────────────────────────────────────────────
const char* Engine::SelfDigest() { return selfDigest_; }

int Engine::IntegrityCheck(const char* expectHex) {
  const int rc = yieyu_integrity_check(expectHex);
  integ_.store(rc);
  if (rc == 0) bb_write(YIEYU_LOG_ERROR, "[11] 完整性校验不通过：自身 so 与构建期摘要不一致");
  return rc;
}

int Engine::KvCount() const { return kv_count(); }
int Engine::StoreCount() const { return storeEntries_.load(); }

// ── [5] 搜歌：控制面只投递关键字，网络在独立线程跑 ────────────────────
int Engine::Search(const char* keyword) {
  if (!keyword) return -1;
  std::string kw(keyword);
  size_t b = 0;
  while (b < kw.size() && (kw[b] == ' ' || kw[b] == '\t' || kw[b] == '\r' || kw[b] == '\n')) ++b;
  kw.erase(0, b);
  while (!kw.empty()) {
    const char c = kw[kw.size() - 1];
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') kw.erase(kw.size() - 1);
    else break;
  }
  if (kw.empty()) return -1;

  {
    std::lock_guard<std::mutex> lk(searchMu_);
    searchKw_ = kw;
    searchPending_ = true;
    if (!searchStarted_) {
      searchStarted_ = true;
      searchQuit_ = false;
      searchTh_ = std::thread([this] { SearchWorker(); });
      searchTh_.detach();   // 进程退出时不 join，避免 std::terminate
    }
  }
  searchCv_.notify_all();
  return 0;
}

void Engine::SearchWorker() {
  for (;;) {
    std::string kw;
    {
      std::unique_lock<std::mutex> lk(searchMu_);
      searchCv_.wait(lk, [this] { return searchQuit_ || searchPending_; });
      if (searchQuit_) return;
      kw.swap(searchKw_);
      searchPending_ = false;
    }
#ifdef YIEYU_HAVE_FETCH
    int len = 0;
    char* js = yieyu_music_search(kw.c_str(), 1, 20, &len);
    std::string body;
    if (js) {
      const size_t n = len > 0 ? (size_t)len : std::strlen(js);
      body.assign(js, n);
      yieyu_fetch_free(js);
    }
    if (body.empty()) body = "[]";
    PushEvent('S', body);
#else
    PushEvent('E', "本次构建没带 libyieyu_fetch.so（搜歌不可用）");
#endif
  }
}

void Engine::PushEvent(char type, const std::string& payload) {
  std::lock_guard<std::mutex> lk(evtMu_);
  evtType_ = type;
  evtPayload_ = payload;
}

// 事件帧（[21] 契约）：40 字节头（magic "YFRM"，channels=事件类型，frames=载荷长度）+ UTF-8 载荷
size_t Engine::DrainEvents(uint8_t* out, size_t cap) {
  if (!out || cap < (size_t)kFrameHeaderBytes) return 0;
  std::lock_guard<std::mutex> lk(evtMu_);
  if (evtType_ == 0) return 0;
  const size_t n = evtPayload_.size();
  if ((size_t)kFrameHeaderBytes + n > cap) {     // 装不下就丢掉，别给 Dart 半截 JSON
    evtType_ = 0;
    evtPayload_.clear();
    return 0;
  }
  codec_write(out, (int)cap, kMagicFrame, (uint16_t)YIEYU_JSB_ABI,
              (uint16_t)(uint8_t)evtType_, 0, (uint32_t)n, ++evtSeq_, 0);
  if (n) std::memcpy(out + kFrameHeaderBytes, evtPayload_.data(), n);
  evtType_ = 0;
  evtPayload_.clear();
  return (size_t)kFrameHeaderBytes + n;
}

int Engine::PlaySong(const char* source, const char* musicId, const char* mid) {
  const char* src = (source && *source) ? source : "wy";
  const char* id = musicId ? musicId : "";
  const char* m = mid ? mid : "";
  if (!*id && !*m) return -1;
  char uri[512];
  std::snprintf(uri, sizeof(uri), "yieyu://song?source=%s&id=%s&mid=%s", src, id, m);
  return PlayUrl(uri);
}

// 下行命令帧（[21] 契约）：40 字节头（magic "YJSC"，channels=op，frames=载荷长度）+ UTF-8 载荷
int Engine::HandleCommand(const uint8_t* frame, size_t len) {
  const int chk = codec_check(frame, (int)len, kMagicCmd);
  if (chk != 0) return chk == -3 ? -3 : -2;
  const uint8_t op = (uint8_t)(codec_u16(frame, kOffChannels) & 0xFFu);
  const uint32_t plen = codec_u32(frame, kOffFrames);
  if ((size_t)kFrameHeaderBytes + (size_t)plen > len) return -4;
  char slot[516];
  const char* payload = "";
  if (plen > 0) {
    const size_t take = plen < sizeof(slot) - 1 ? plen : sizeof(slot) - 1;
    std::memcpy(slot, frame + kFrameHeaderBytes, take);
    slot[take] = 0;
    payload = slot;
  }
  // op 编号 = idl/jsb.fbs 的 enum Op（0=NONE 起算）；Dart 的 enum JsbOp 与这里必须同序
  switch (op) {
    case 0: return 0;                                    // NONE
    case 1: ResumeAudio(); return 0;                     // PLAY（继续当前曲目）
    case 2: PauseAudio();  return 0;                     // PAUSE
    case 3: return 0;                                    // NEXT（放什么归模块 1）
    case 4: return 0;                                    // PREV
    case 5: return Search(payload);                      // SEARCH
    case 6: return SeekMs(std::strtoll(payload, nullptr, 10));  // SEEK
    case 7: return 0;                                    // EQ_PRESET_NEXT
    case 8: return 0;                                    // SET_EQ_BAND
    case 9: return 0;                                    // LICENSE
    case 10: return 0;                                   // LOAD_PLUGIN
    default: return 0;
  }
}

int Engine::Snapshot(Yieyu_Snapshot* out) {
  Player& p = Player::Instance();
  out->playing    = p.IsPlaying() ? 1 : 0;
  out->sampleRate = p.SampleRate();
  out->positionMs = p.PositionMs();
  out->durationMs = p.DurationMs();
  out->peakL      = p.PeakL();
  out->peakR      = p.PeakR();
  out->eqPoints   = eq31_points(out->eqDb);
  return 0;
}

void Engine::OnSystemAction(int actionId) {
  // 模块22 只转发，不做判定
  switch (actionId) {
    case 0: ResumeAudio(); break;   // play
    case 1: PauseAudio();  break;   // pause
    default: break;                 // next/prev 由上层（模块 1）决定播什么
  }
}

void Engine::Stats(char* out, size_t cap) {
  Player& p = Player::Instance();
  std::snprintf(out, cap,
                "state=%d sr=%d pos=%lld dur=%lld st=%d underrun=%d ring=%zu/%zu wfail=%d chunks=%lld "
                "lic=%d integ=%d kv=%d store=%d gyro=%d dsd=%d resamp=%d target=%d plug=%d",
                (int)state_.load(), (int)p.SampleRate(), (long long)p.PositionMs(),
                (long long)p.DurationMs(), p.State(), (int)yieyu_aaudio_underruns(),
                p.RingReadable(), p.RingCapacity(), (int)p.RingWriteFails(),
                (long long)p.DecodedChunks(), (int)lic_.load(), (int)integ_.load(),
                kv_count(), (int)storeEntries_.load(), gyro_is_available(),
                p.IsDsd() ? 1 : 0, p.IsResampling() ? 1 : 0, p.TargetRate(), plugin_ready());
}

}  // namespace yieyu
