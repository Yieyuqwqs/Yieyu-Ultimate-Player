// 总指挥。真正的"业务决策"只有三件：能不能播、播什么、出错怎么办。
#include "engine.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "../jsb/jsb.h"
#include "../common/log.h"
#include "../common/blackbox.h"
#include "../common/app_paths.h"
#include "../dsp/dsp.h"
#include "../sensor/gyro_asensor.h"
#include "../store/sqlite_kv.h"
#include "../audio/aaudio_sink.h"
#include "../script/lua_sandbox.h"
#include "player.h"

// [5]/[9] 搜歌出口：Go 编的 libyieyu_fetch.so（与 Lua 的 http_* 是同一份）
#ifdef YIEYU_HAVE_FETCH
extern "C" {
char* yieyu_music_search(const char* keyword, int page, int limit, int* outLen);
void  yieyu_fetch_free(char* p);
}
#endif

// [10] 验签：Rust + ring 编的 libyieyu_license.so（以前只编、不进包，也从来没被调用）
// ★ 不做静态链：Rust cdylib 没有 SONAME，链进 core 会把绝对路径写进 DT_NEEDED，
//   装机 dlopen 必炸（CI 15.5 就是拦这个）。运行时 dlopen 更稳，缺库也不崩。
#ifdef YIEYU_HAVE_LICENSE
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
    // 连私有目录都写不进：没有日志可落，但 App 仍要起来（UI 静默，原因在 dmesg 之外看不到）
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
  //    以前是 `const bool licensed = true;` —— 也就是说这整个模块是摆设。
  //    现在真的去问 Rust/ring 编的库：没有库、或库里没有内嵌公钥，都在 Stats 与
  //    黑匣子里留痕（本地自用阶段不熔断，但绝不再假装"验过了"）。
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
    bb_write(YIEYU_LOG_ERROR, "[10] 验签失败 → 熔断（停解码/清环/置灰），trace=%s", bb_trace());
    LOGW("license fail → 熔断：停解码、清 PCM 环、UI 置灰（不崩进程）");
    return 0;  // 注意：返回 0，UI 仍要起来（退款提示页）
  }

  // ② [20] 打开 SQLite（唯一写者，WAL）
  if (sqlite_kv_open(yp_data()) != 0) {
    state_.store(State::kFault);
    bb_write(YIEYU_LOG_FATAL, "[20] SQLite 打开失败，dir=%s", filesDir ? filesDir : "?");
    return -2;
  }
  sqlite_kv_load_session();

  // ③ [23] 陀螺仪（8D 用；没有传感器就静默降级，不外抛）
  gyro_start(200 /* Hz */);

  // ④ [7] Lua 沙箱：**必须在这里点火**。
  //    上一版全仓没人调 lua_sandbox_init() → g_L 永远是 nullptr → 任何脚本调用都返回 -1，
  //    脚本"导入成功"却永远执行不了。启动时再扫一遍脚本目录，App 重启后不用重新导入。
  if (lua_sandbox_init(yp_scripts()) != 0) {
    bb_write(YIEYU_LOG_ERROR, "[7] Lua 沙箱初始化失败（脚本将不可用）dir=%s", yp_scripts());
  } else {
    int loaded = 0, rejected = 0;
    lua_sandbox_reload(yp_scripts(), &loaded, &rejected);
    if (loaded == 0 && rejected > 0) {
      bb_write(YIEYU_LOG_ERROR, "[7] 启动扫描：登记 0 个、被拒 %d 个（目录 %s）", rejected, yp_scripts());
    }
  }

  state_.store(State::kReady);
  LOGI("engine ready abi=%s sr=%d", abi, Player::Instance().SampleRate());
  return 0;
}

void Engine::Shutdown() {
  // 搜歌线程先停：它可能正阻塞在网络上，只置标志 + 唤醒，join 会卡住退出。
  // 线程本身在创建时就 detach 了，这里不需要也不允许再 join。
  {
    std::lock_guard<std::mutex> lk(searchMu_);
    searchQuit_ = true;
    searchPending_ = false;
  }
  searchCv_.notify_all();

  Player::Instance().Shutdown();
  gyro_stop();
  sqlite_kv_close();
  lua_sandbox_shutdown();
  bb_flush();
  bb_shutdown();
  state_.store(State::kIdle);
}

int Engine::PlayUrl(const char* uri) { return Player::Instance().PlayUrl(uri); }
void Engine::PauseAudio() { Player::Instance().Pause(); }
void Engine::ResumeAudio() { Player::Instance().Resume(); }
void Engine::StopAudio() { Player::Instance().Stop(); }
int  Engine::SeekMs(int64_t ms) { return Player::Instance().SeekMs(ms); }
void Engine::SetVolume(float v) { Player::Instance().SetVolume(v); }
void Engine::PlayerError(char* out, size_t cap) const { Player::Instance().LastError(out, cap); }
void Engine::SetDsp(bool on) { Player::Instance().SetDsp(on); }
void Engine::SetEqGain(int band, float db) { Player::Instance().SetEqGain(band, db); }
void Engine::SetTone(bool tube, bool vinyl, bool lpf, float drive) {
  Player::Instance().SetTone(tube, vinyl, lpf, drive);
}

// ── [5] 搜歌：控制面只投递关键字，网络在独立线程跑 ────────────────────
int Engine::Search(const char* keyword) {
  if (!keyword) return -1;
  std::string kw(keyword);
  // 去掉首尾空白（中文关键字 copy 过来也安全）
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
    bb_write(YIEYU_LOG_ERROR, "[5] 搜歌 %.40s：%zu 字节", kw.c_str(), body.size());
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

// 事件帧：[0] 类型（'S' 搜索结果 / 'E' 错误文案）[1..4] 小端长度 [5..] UTF-8
size_t Engine::DrainEvents(uint8_t* out, size_t cap) {
  if (!out || cap < 5) return 0;
  std::lock_guard<std::mutex> lk(evtMu_);
  if (evtType_ == 0) return 0;
  const size_t n = evtPayload_.size();
  if (5 + n > cap) {                     // 装不下就丢掉，别给 Dart 半截 JSON
    evtType_ = 0;
    evtPayload_.clear();
    return 0;
  }
  out[0] = (uint8_t)evtType_;
  out[1] = (uint8_t)(n & 0xff);
  out[2] = (uint8_t)((n >> 8) & 0xff);
  out[3] = (uint8_t)((n >> 16) & 0xff);
  out[4] = (uint8_t)((n >> 24) & 0xff);
  if (n) std::memcpy(out + 5, evtPayload_.data(), n);
  evtType_ = 0;
  evtPayload_.clear();
  return 5 + n;
}

int Engine::PlaySong(const char* source, const char* musicId, const char* mid) {
  const char* src = (source && *source) ? source : "wy";
  const char* id = musicId ? musicId : "";
  const char* m = mid ? mid : "";
  if (!*id && !*m) return -1;
  char uri[512];
  std::snprintf(uri, sizeof(uri), "yieyu://song?source=%s&id=%s&mid=%s", src, id, m);
  return Player::Instance().PlayUrl(uri);
}

// 下行命令帧：'YJSC' + op  (+3 字节预留) + UTF-8 载荷（与 app/lib/jsb/jsb_ops.dart 一致）
int Engine::HandleCommand(const uint8_t* fbs, size_t len) {
  if (!fbs || len < 8) return -1;
  if (!(fbs[0] == 'Y' && fbs[1] == 'J' && fbs[2] == 'S' && fbs[3] == 'C')) return -2;
  const uint8_t op = fbs[4];
  const char* payload = (len > 8) ? reinterpret_cast<const char*>(fbs + 8) : "";
  switch (op) {
    case 0: ResumeAudio(); return 0;                     // play（继续当前曲目）
    case 1: PauseAudio();  return 0;                     // pause
    case 2: return 0;                                    // next（放什么归模块 1）
    case 3: return 0;                                    // prev
    case 4: return Search(payload);                      // search
    case 5: return SeekMs(std::strtoll(payload, nullptr, 10));
    case 6: return 0;                                    // eqPresetNext
    case 7: return 0;                                    // setEqBand
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
  // [17] 31 段 EQ 的真实曲线（以前 eqPoints 写死 31、eqDb 一个字节都没填 → 曲线永远平）
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
                "state=%d sr=%d pos=%lld dur=%lld st=%d underrun=%d ring=%zu/%zu wfail=%d chunks=%lld lic=%d",
                (int)state_.load(), (int)p.SampleRate(), (long long)p.PositionMs(),
                (long long)p.DurationMs(), p.State(), (int)yieyu_aaudio_underruns(),
                p.RingReadable(), p.RingCapacity(), (int)p.RingWriteFails(),
                (long long)p.DecodedChunks(), (int)lic_.load());
}

}  // namespace yieyu
