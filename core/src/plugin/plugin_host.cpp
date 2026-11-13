// [8] YPFX 插件宿主实现：解析头部 + 极小栈式 VM。
#include "plugin_host.h"
#include "abi.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <string>
#include <vector>

#include "../common/log.h"

namespace {

char g_err[192] = {0};
void set_err(const char* s) { std::snprintf(g_err, sizeof(g_err), "%s", s); }

uint8_t  g_kind = 0;
std::vector<uint8_t> g_code;
float    g_state[yieyu::plugin::kStateSlots] = {0};
bool     g_ready = false;
int      g_count = 0;
char     g_name[128] = {0};
float    g_lfoHz = 5.0f;
int      g_rate = 0;

inline float f32_from(const uint8_t* p) { float v = 0.0f; std::memcpy(&v, p, 4); return v; }
inline uint32_t u32_from(const uint8_t* p) { uint32_t v = 0; std::memcpy(&v, p, 4); return v; }

// 执行一次插件 → 返回一个采样点的增益；失败返回 false
bool run_once(float* out) {
  using namespace yieyu::plugin;
  float stack[64];
  int sp = 0;
  const size_t n = g_code.size();
  size_t pc = 0;
  int steps = 0;
  auto pushf = [&](float v) -> bool { if (sp >= 64) return false; stack[sp++] = v; return true; };
  auto popf = [&](float* v) -> bool { if (sp <= 0) return false; *v = stack[--sp]; return true; };
  auto popi = [&](int* v) -> bool {
    float f = 0.0f;
    if (!popf(&f)) return false;
    *v = (int)f;
    return (*v >= 0 && *v < kStateSlots);
  };

  while (pc < n) {
    if (++steps > kMaxSteps) { set_err("插件指令数超上限（死循环？）"); return false; }
    const uint8_t op = g_code[pc++];
    switch (op) {
      case kOpPushI: {
        if (pc + 4 > n) { set_err("PUSHI 越界"); return false; }
        if (!pushf((float)u32_from(&g_code[pc]))) return false;
        pc += 4;
        break;
      }
      case kOpPushF: {
        if (pc + 4 > n) { set_err("PUSHF 越界"); return false; }
        if (!pushf(f32_from(&g_code[pc]))) return false;
        pc += 4;
        break;
      }
      case kOpLoad: {
        int addr = 0;
        if (!popi(&addr)) { set_err("LOAD 地址越界"); return false; }
        if (!pushf(g_state[addr])) return false;
        break;
      }
      case kOpStore: {
        int addr = 0;
        float v = 0.0f;
        if (!popi(&addr)) { set_err("STORE 地址越界"); return false; }
        if (!popf(&v)) { set_err("STORE 缺值"); return false; }
        g_state[addr] = v;
        break;
      }
      case kOpAdd: case kOpSub: case kOpMul: {
        float b = 0.0f, a = 0.0f;
        if (!popf(&b) || !popf(&a)) { set_err("算术栈不足"); return false; }
        if (!pushf(op == kOpAdd ? a + b : (op == kOpSub ? a - b : a * b))) return false;
        break;
      }
      case kOpFloor: {
        float a = 0.0f;
        if (!popf(&a)) { set_err("FLOOR 栈空"); return false; }
        if (!pushf(std::floor(a))) return false;
        break;
      }
      case kOpDup: {
        if (sp <= 0) { set_err("DUP 栈空"); return false; }
        if (!pushf(stack[sp - 1])) return false;
        break;
      }
      case kOpDrop:
        if (sp <= 0) { set_err("DROP 栈空"); return false; }
        --sp;
        break;
      case kOpRet:
        if (!popf(out)) { set_err("RET 栈空"); return false; }
        return true;
      default:
        set_err("未知指令");
        return false;
    }
  }
  set_err("插件没有 RET");
  return false;
}

}  // namespace

extern "C" {

int plugin_host_init(const char* pluginDir) {
  g_err[0] = 0;
  g_ready = false;
  g_count = 0;
  if (!pluginDir || !*pluginDir) { set_err("没有插件目录"); return -1; }
  DIR* d = ::opendir(pluginDir);
  if (!d) { set_err("插件目录打不开"); return -2; }
  std::string chosen;
  struct dirent* e = nullptr;
  while ((e = ::readdir(d)) != nullptr) {
    const size_t len = std::strlen(e->d_name);
    if (len < 6 || std::strcmp(e->d_name + len - 5, ".ypfx") != 0) continue;
    ++g_count;
    if (chosen.empty()) chosen = std::string(pluginDir) + "/" + e->d_name;
  }
  ::closedir(d);
  if (chosen.empty()) { set_err("没有 .ypfx 插件（正常：插件是可选的）"); return -3; }
  return plugin_load(chosen.c_str());
}

int plugin_host_reload(const char* pluginDir) {
  plugin_shutdown();
  return plugin_host_init(pluginDir);
}

int plugin_load(const char* path) {
  g_ready = false;
  FILE* f = std::fopen(path, "rb");
  if (!f) { set_err("插件打不开"); return -1; }
  std::fseek(f, 0, SEEK_END);
  const long size = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  if (size < 20 || size > 1024 * 1024) { std::fclose(f); set_err("插件大小异常"); return -2; }
  std::vector<uint8_t> buf((size_t)size);
  const size_t got = std::fread(buf.data(), 1, buf.size(), f);
  std::fclose(f);
  if (got != buf.size()) { set_err("插件读不满"); return -3; }

  using namespace yieyu::plugin;
  if (std::memcmp(buf.data(), kMagic, 4) != 0) { set_err("不是 YPFX 插件"); return -4; }
  if (buf[4] != kVersion) { set_err("插件版本不支持"); return -5; }
  g_kind = buf[5];
  const float lfoHz = f32_from(&buf[6]);
  g_lfoHz = (lfoHz > 0.01f && lfoHz < 40.0f) ? lfoHz : 5.0f;
  const float depth = f32_from(&buf[10]);
  const uint32_t codeLen = u32_from(&buf[14]);
  if (codeLen == 0 || codeLen > (uint32_t)kMaxCodeBytes || 18 + codeLen != buf.size()) {
    set_err("插件代码段长度不对");
    return -6;
  }
  g_code.assign(buf.begin() + 18, buf.begin() + 18 + (size_t)codeLen);
  std::memset(g_state, 0, sizeof(g_state));
  g_state[8] = depth;
  std::snprintf(g_name, sizeof(g_name), "%s", path);

  // 采样率相关的初始化在 plugin_process 第一次调用时完成（那时才知道声卡采样率）
  g_ready = true;
  return 0;
}

int plugin_on_rate(int sampleRate) {
  if (!g_ready || sampleRate <= 0) return -1;
  g_rate = sampleRate;
  g_state[0] = g_lfoHz / (float)sampleRate;   // step
  g_state[4] = 0.0f;                          // phase 归零
  return 0;
}

int plugin_ready(void) { return g_ready ? 1 : 0; }
int plugin_count(void) { return g_count; }

int plugin_process(float* interleaved, int frames) {
  if (!g_ready || !interleaved || frames <= 0) return -1;
  if (g_rate <= 0) { set_err("插件没拿到采样率"); return -3; }
  for (int i = 0; i < frames; ++i) {
    float g = 1.0f;
    if (!run_once(&g)) {
      g_ready = false;
      return -2;
    }
    if (g > 2.0f) g = 2.0f;
    if (g < 0.0f) g = 0.0f;
    interleaved[i * 2] *= g;
    interleaved[i * 2 + 1] *= g;
  }
  return 0;
}

const char* plugin_error(void) { return g_err; }

void plugin_shutdown(void) {
  g_code.clear();
  std::memset(g_state, 0, sizeof(g_state));
  g_ready = false;
  g_count = 0;
  g_kind = 0;
  g_name[0] = 0;
}

}  // extern "C"
