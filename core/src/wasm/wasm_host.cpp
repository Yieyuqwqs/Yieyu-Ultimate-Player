// [8] wasm MVP 解释器实现（只支持 tools/mkwasm.mjs 会产出的指令子集 + 少量常用指令）。
//
// 结构：
//   · 解析 type/function/memory/export/code 五个段（有 import 段直接拒绝：插件不许要宿主要东西）
//   · 载入时预扫描每个函数体，建"block/loop/if ↔ else/end"配对表（br 靠它跳）
//   · 执行：值栈（union）+ 局部变量 + 控制栈；每个函数最多 5000 万条指令，防死循环
//
// 铁律：这里不打印任何东西（成功静默），出错只写黑盒；只在解码线程调用。
#include "wasm_host.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <vector>

#include "../common/log.h"

namespace {

char g_err[192] = {0};
void set_err(const char* s) { std::snprintf(g_err, sizeof(g_err), "%s", s); }

struct Val {
  uint32_t i32 = 0;
  float f32 = 0.0f;
  int type = 0;   // 0=i32 1=f32
};

struct Func {
  std::vector<uint8_t> localsType;   // 除参数外的局部变量类型
  uint32_t paramCount = 0;
  uint32_t resultCount = 0;
  std::vector<uint8_t> code;         // 函数体（含末尾 end）
  std::vector<uint32_t> matchEnd;    // 按"指令字节偏移"索引：block/loop/if → 对应 end
  std::vector<uint32_t> matchElse;   // if → else（无 else 时为 0）
};

struct Module {
  std::vector<Func> funcs;
  std::vector<uint8_t> memory;       // 64KB
  std::vector<std::string> exportNames;
  std::vector<uint32_t> exportFuncs; // 与 exportNames 对应，0xFFFFFFFF 表示内存
  uint32_t entryAbi = 0xFFFFFFFF;
  uint32_t entryInit = 0xFFFFFFFF;
  uint32_t entryProcess = 0xFFFFFFFF;
};

Module g_mod;
bool g_ready = false;
float g_pendingProcess[4096 * 2] = {0};
int g_count = 0;

// ── LEB128 ────────────────────────────────────────────────────────
bool rd_uleb(const uint8_t* p, size_t n, size_t* at, uint64_t* out) {
  uint64_t v = 0;
  int shift = 0;
  while (*at < n) {
    const uint8_t b = p[(*at)++];
    v |= (uint64_t)(b & 0x7f) << shift;
    if (!(b & 0x80)) { *out = v; return true; }
    shift += 7;
    if (shift > 63) return false;
  }
  return false;
}
bool rd_sleb(const uint8_t* p, size_t n, size_t* at, int64_t* out) {
  int64_t v = 0;
  int shift = 0;
  uint8_t b = 0;
  do {
    if (*at >= n) return false;
    b = p[(*at)++];
    v |= (int64_t)(b & 0x7f) << shift;
    shift += 7;
  } while (b & 0x80);
  if (shift < 64 && (b & 0x40)) v |= -(1LL << shift);
  *out = v;
  return true;
}

// ── 预扫描：把 block/loop/if 与 else/end 配对（br 靠它跳）────────────
bool prescan(Func* f) {
  const size_t n = f->code.size();
  f->matchEnd.assign(n, 0);
  f->matchElse.assign(n, 0);
  std::vector<uint32_t> stack;
  size_t at = 0;
  while (at < n) {
    const uint32_t here = (uint32_t)at;
    const uint8_t op = f->code[at++];
    uint64_t u = 0;
    int64_t s = 0;
    switch (op) {
      case 0x02: case 0x03: case 0x04:      // block / loop / if
        ++at;                               // blocktype
        stack.push_back(here);
        break;
      case 0x05: {                          // else
        if (stack.empty()) return false;
        const uint32_t owner = stack.back();
        if (f->code[owner] != 0x04) return false;
        f->matchElse[owner] = here;
        break;
      }
      case 0x0b: {                          // end
        if (stack.empty()) { at = n; break; }
        const uint32_t owner = stack.back();
        stack.pop_back();
        f->matchEnd[owner] = here;
        break;
      }
      case 0x0c: case 0x0d:                 // br / br_if
      case 0x20: case 0x21:                 // local.get / local.set
        if (!rd_uleb(f->code.data(), n, &at, &u)) return false;
        break;
      case 0x41:
        if (!rd_sleb(f->code.data(), n, &at, &s)) return false;
        break;
      case 0x43:
        at += 4;
        break;
      case 0x28: case 0x2a: case 0x36: case 0x38:
        if (!rd_uleb(f->code.data(), n, &at, &u)) return false;
        if (!rd_uleb(f->code.data(), n, &at, &u)) return false;
        break;
      default:
        break;                              // 其余都是无立即数指令
    }
  }
  return true;
}

// ── 解析模块 ─────────────────────────────────────────────────────
bool parse_module(const uint8_t* b, size_t n) {
  if (n < 8 || std::memcmp(b, "\0asm", 4) != 0 || b[4] != 1) { set_err("不是 wasm MVP 模块"); return false; }
  g_mod = Module();
  g_mod.memory.assign(64 * 1024, 0);
  std::vector<std::vector<uint8_t>> types;    // 每个 type 的参数/结果类型序列：先参数后结果，用 count 记录
  std::vector<uint32_t> typeParams, typeResults;
  std::vector<uint32_t> funcTypes;
  std::vector<uint32_t> codeOffsets, codeSizes;

  size_t at = 8;
  while (at + 2 <= n) {
    const uint8_t id = b[at++];
    uint64_t size = 0;
    if (!rd_uleb(b, n, &at, &size)) return false;
    const size_t secEnd = at + (size_t)size;
    if (secEnd > n) { set_err("段越界"); return false; }
    if (id == 2) { set_err("插件不许 import（宿主不提供任何能力）"); return false; }
    if (id == 1) {                              // type
      uint64_t cnt = 0;
      if (!rd_uleb(b, n, &at, &cnt)) return false;
      for (uint64_t i = 0; i < cnt; ++i) {
        if (at >= secEnd || b[at++] != 0x60) return false;
        uint64_t np = 0, nr = 0;
        if (!rd_uleb(b, n, &at, &np)) return false;
        at += (size_t)np;                        // 只支持数值类型（1 字节）
        if (!rd_uleb(b, n, &at, &nr)) return false;
        at += (size_t)nr;
        types.push_back({});
        typeParams.push_back((uint32_t)np);
        typeResults.push_back((uint32_t)nr);
      }
    } else if (id == 3) {                       // function
      uint64_t cnt = 0;
      if (!rd_uleb(b, n, &at, &cnt)) return false;
      for (uint64_t i = 0; i < cnt; ++i) {
        uint64_t t = 0;
        if (!rd_uleb(b, n, &at, &t)) return false;
        funcTypes.push_back((uint32_t)t);
      }
    } else if (id == 5) {                       // memory
      uint64_t cnt = 0;
      if (!rd_uleb(b, n, &at, &cnt)) return false;
      for (uint64_t i = 0; i < cnt; ++i) {
        if (at >= secEnd) return false;
        const uint8_t flags = b[at++];
        uint64_t minPages = 0;
        if (!rd_uleb(b, n, &at, &minPages)) return false;
        if (flags & 1) { uint64_t mx = 0; if (!rd_uleb(b, n, &at, &mx)) return false; }
        (void)minPages;                          // 统一给 1 页（64KB）
      }
    } else if (id == 7) {                       // export
      uint64_t cnt = 0;
      if (!rd_uleb(b, n, &at, &cnt)) return false;
      for (uint64_t i = 0; i < cnt; ++i) {
        uint64_t nlen = 0;
        if (!rd_uleb(b, n, &at, &nlen)) return false;
        std::string name(reinterpret_cast<const char*>(b + at), (size_t)nlen);
        at += (size_t)nlen;
        if (at >= secEnd) return false;
        const uint8_t kind = b[at++];
        uint64_t idx = 0;
        if (!rd_uleb(b, n, &at, &idx)) return false;
        g_mod.exportNames.push_back(name);
        g_mod.exportFuncs.push_back(kind == 0 ? (uint32_t)idx : 0xFFFFFFFFu);
      }
    } else if (id == 10) {                      // code
      uint64_t cnt = 0;
      if (!rd_uleb(b, n, &at, &cnt)) return false;
      for (uint64_t i = 0; i < cnt; ++i) {
        uint64_t sz = 0;
        if (!rd_uleb(b, n, &at, &sz)) return false;
        codeOffsets.push_back((uint32_t)at);
        codeSizes.push_back((uint32_t)sz);
        at += (size_t)sz;
      }
    }
    at = secEnd;
  }

  if (funcTypes.size() != codeOffsets.size()) { set_err("function/code 段数量不一致"); return false; }
  for (size_t i = 0; i < funcTypes.size(); ++i) {
    Func f;
    const uint32_t t = funcTypes[i];
    if (t >= typeParams.size()) { set_err("函数类型索引越界"); return false; }
    f.paramCount = typeParams[t];
    f.resultCount = typeResults[t];
    const uint8_t* p = b + codeOffsets[i];
    size_t at2 = 0;
    uint64_t groups = 0;
    if (!rd_uleb(p, codeSizes[i], &at2, &groups)) { set_err("局部变量段坏"); return false; }
    for (uint64_t g = 0; g < groups; ++g) {
      uint64_t cnt = 0;
      if (!rd_uleb(p, codeSizes[i], &at2, &cnt)) return false;
      if (at2 >= codeSizes[i]) return false;
      const uint8_t vt = p[at2++];
      for (uint64_t k = 0; k < cnt; ++k) f.localsType.push_back(vt);
    }
    f.code.assign(p + at2, p + codeSizes[i]);
    if (f.code.empty() || f.code.back() != 0x0b) { set_err("函数体没有收尾 end"); return false; }
    if (!prescan(&f)) { set_err("控制结构不配对"); return false; }
    g_mod.funcs.push_back(std::move(f));
  }

  for (size_t i = 0; i < g_mod.exportNames.size(); ++i) {
    const std::string& nm = g_mod.exportNames[i];
    if (g_mod.exportFuncs[i] == 0xFFFFFFFFu) continue;
    if (nm == "abi") g_mod.entryAbi = g_mod.exportFuncs[i];
    else if (nm == "init") g_mod.entryInit = g_mod.exportFuncs[i];
    else if (nm == "process") g_mod.entryProcess = g_mod.exportFuncs[i];
    else if (nm == "next") g_mod.entryNext = g_mod.exportFuncs[i];
  }
  if (g_mod.entryNext == 0xFFFFFFFFu && g_mod.entryProcess == 0xFFFFFFFFu) {
    set_err("插件既没有 next 也没有 process 导出");
    return false;
  }
  return true;
}

// ── 执行 ─────────────────────────────────────────────────────────
struct Ctl {
  uint32_t off;       // 该 block/loop/if 的指令偏移
  uint32_t stackH;    // 进入时的值栈高度
  int kind;           // 2=block 3=loop 4=if
};

bool exec(uint32_t fi, const std::vector<Val>& args, Val* result) {
  if (fi >= g_mod.funcs.size()) { set_err("函数索引越界"); return false; }
  Func& f = g_mod.funcs[fi];
  std::vector<Val> stack;
  stack.reserve(256);
  std::vector<Val> locals(f.paramCount + f.localsType.size());
  for (uint32_t i = 0; i < f.paramCount && i < args.size(); ++i) locals[i] = args[i];
  std::vector<Ctl> ctl;
  const size_t codeN = f.code.size();
  size_t pc = 0;
  uint64_t steps = 0;
  const uint64_t kMaxSteps = 50ull * 1000 * 1000;

  auto push_i = [&](uint32_t v) { Val x; x.type = 0; x.i32 = v; stack.push_back(x); };
  auto push_f = [&](float v) { Val x; x.type = 1; x.f32 = v; stack.push_back(x); };

  while (pc < codeN) {
    if (++steps > kMaxSteps) { set_err("插件执行超过指令上限（疑似死循环）"); return false; }
    const uint8_t op = f.code[pc];
    switch (op) {
      case 0x02: case 0x03: case 0x04:
        ctl.push_back({(uint32_t)pc, (uint32_t)stack.size(), op == 0x02 ? 2 : (op == 0x03 ? 3 : 4)});
        pc += 2;
        break;
      case 0x05: {                              // else：跳到 end
        if (ctl.empty()) { set_err("else 无主"); return false; }
        pc = f.matchEnd[ctl.back().off] + 1;
        break;
      }
      case 0x0b: {                              // end
        if (!ctl.empty()) ctl.pop_back();
        ++pc;
        if (pc >= codeN) return true;           // 函数结束
        break;
      }
      case 0x0c: case 0x0d: {                   // br / br_if
        size_t q = pc + 1;
        uint64_t depth = 0;
        if (!rd_uleb(f.code.data(), codeN, &q, &depth)) { set_err("br 立即数坏"); return false; }
        bool take = true;
        if (op == 0x0d) {
          if (stack.empty() || stack.back().type != 0) { set_err("br_if 条件类型错"); return false; }
          take = stack.back().i32 != 0;
          stack.pop_back();
        }
        if (!take) { pc = (size_t)q; break; }
        if (depth >= ctl.size()) return true;   // 跳到函数外 = 返回
        const Ctl target = ctl[ctl.size() - 1 - (size_t)depth];
        ctl.resize(ctl.size() - (size_t)depth - 1);
        stack.resize(target.stackH);
        if (target.kind == 3) { pc = target.off + 2; }          // loop：回到循环头
        else { pc = f.matchEnd[target.off] + 1; }               // block/if：跳到 end 之后
        break;
      }
      case 0x0f: {                              // return
        if (!stack.empty() && result) *result = stack.back();
        return true;
      }
      case 0x1a:                                // drop
        if (stack.empty()) { set_err("drop 栈空"); return false; }
        stack.pop_back();
        ++pc;
        break;
      case 0x20: case 0x21: {
        size_t q = pc + 1;
        uint64_t idx = 0;
        if (!rd_uleb(f.code.data(), codeN, &q, &idx)) { set_err("local 索引坏"); return false; }
        if (op == 0x20) {
          if (idx >= locals.size()) { set_err("local.get 越界"); return false; }
          stack.push_back(locals[(size_t)idx]);
        } else {
          if (idx >= locals.size() || stack.empty()) { set_err("local.set 越界"); return false; }
          locals[(size_t)idx] = stack.back();
          stack.pop_back();
        }
        pc = (size_t)q;
        break;
      }
      case 0x41: {
        size_t q = pc + 1;
        int64_t v = 0;
        if (!rd_sleb(f.code.data(), codeN, &q, &v)) { set_err("i32.const 坏"); return false; }
        push_i((uint32_t)v);
        pc = (size_t)q;
        break;
      }
      case 0x43: {
        if (pc + 5 > codeN) { set_err("f32.const 坏"); return false; }
        float v = 0.0f;
        std::memcpy(&v, f.code.data() + pc + 1, 4);
        push_f(v);
        pc += 5;
        break;
      }
      case 0x28: case 0x2a: {                   // i32.load / f32.load
        size_t q = pc + 1;
        uint64_t align = 0, off = 0;
        if (!rd_uleb(f.code.data(), codeN, &q, &align)) return false;
        if (!rd_uleb(f.code.data(), codeN, &q, &off)) return false;
        if (stack.empty() || stack.back().type != 0) { set_err("load 地址类型错"); return false; }
        const uint32_t addr = stack.back().i32 + (uint32_t)off;
        stack.pop_back();
        if (addr + 4 > g_mod.memory.size()) { set_err("load 越界"); return false; }
        if (op == 0x28) {
          uint32_t v = 0;
          std::memcpy(&v, g_mod.memory.data() + addr, 4);
          push_i(v);
        } else {
          float v = 0.0f;
          std::memcpy(&v, g_mod.memory.data() + addr, 4);
          push_f(v);
        }
        pc = (size_t)q;
        break;
      }
      case 0x36: case 0x38: {                   // i32.store / f32.store
        size_t q = pc + 1;
        uint64_t align = 0, off = 0;
        if (!rd_uleb(f.code.data(), codeN, &q, &align)) return false;
        if (!rd_uleb(f.code.data(), codeN, &q, &off)) return false;
        if (stack.size() < 2) { set_err("store 栈不够"); return false; }
        const Val val = stack.back();
        stack.pop_back();
        if (stack.back().type != 0) { set_err("store 地址类型错"); return false; }
        const uint32_t addr = stack.back().i32 + (uint32_t)off;
        stack.pop_back();
        if (addr + 4 > g_mod.memory.size()) { set_err("store 越界"); return false; }
        if (op == 0x36) {
          const uint32_t v = val.i32;
          std::memcpy(g_mod.memory.data() + addr, &v, 4);
        } else {
          const float v = val.f32;
          std::memcpy(g_mod.memory.data() + addr, &v, 4);
        }
        pc = (size_t)q;
        break;
      }
      case 0x4e: case 0x48: case 0x4a: case 0x4c: {   // i32 比较
        if (stack.size() < 2) { set_err("比较栈不够"); return false; }
        const Val b = stack.back(); stack.pop_back();
        const Val a = stack.back(); stack.pop_back();
        if (a.type != 0 || b.type != 0) { set_err("i32 比较类型错"); return false; }
        int32_t x = (int32_t)a.i32, y = (int32_t)b.i32;
        bool r = false;
        if (op == 0x4e) r = x >= y;
        else if (op == 0x48) r = x < y;
        else if (op == 0x4a) r = x > y;
        else r = x <= y;
        push_i(r ? 1u : 0u);
        ++pc;
        break;
      }
      case 0x5d: case 0x60: {                   // f32.lt / f32.ge
        if (stack.size() < 2) { set_err("f32 比较栈不够"); return false; }
        const Val b = stack.back(); stack.pop_back();
        const Val a = stack.back(); stack.pop_back();
        if (a.type != 1 || b.type != 1) { set_err("f32 比较类型错"); return false; }
        push_i((op == 0x5d ? (a.f32 < b.f32) : (a.f32 >= b.f32)) ? 1u : 0u);
        ++pc;
        break;
      }
      case 0x6a: case 0x6b: case 0x6c: {        // i32 运算
        if (stack.size() < 2) { set_err("i32 运算栈不够"); return false; }
        const Val b = stack.back(); stack.pop_back();
        const Val a = stack.back(); stack.pop_back();
        if (a.type != 0 || b.type != 0) { set_err("i32 运算类型错"); return false; }
        uint32_t r = 0;
        if (op == 0x6a) r = a.i32 + b.i32;
        else if (op == 0x6b) r = a.i32 - b.i32;
        else r = a.i32 * b.i32;
        push_i(r);
        ++pc;
        break;
      }
      case 0x92: case 0x93: case 0x94: case 0x95: {   // f32 运算
        if (stack.size() < 2) { set_err("f32 运算栈不够"); return false; }
        const Val b = stack.back(); stack.pop_back();
        const Val a = stack.back(); stack.pop_back();
        if (a.type != 1 || b.type != 1) { set_err("f32 运算类型错"); return false; }
        float r = 0.0f;
        if (op == 0x92) r = a.f32 + b.f32;
        else if (op == 0x93) r = a.f32 - b.f32;
        else if (op == 0x94) r = a.f32 * b.f32;
        else r = (b.f32 != 0.0f) ? a.f32 / b.f32 : 0.0f;
        push_f(r);
        ++pc;
        break;
      }
      case 0x8e: {                              // f32.floor
        if (stack.empty() || stack.back().type != 1) { set_err("floor 类型错"); return false; }
        const float v = std::floor(stack.back().f32);
        stack.pop_back();
        push_f(v);
        ++pc;
        break;
      }
      case 0xb2: {                              // f32.convert_i32_s
        if (stack.empty() || stack.back().type != 0) { set_err("i32→f32 类型错"); return false; }
        const float v = (float)(int32_t)stack.back().i32;
        stack.pop_back();
        push_f(v);
        ++pc;
        break;
      }
      default:
        set_err("插件用了不支持的指令");
        return false;
    }
  }
  return true;
}

bool call_entry(uint32_t fi, const std::vector<Val>& args, Val* result) { return exec(fi, args, result); }

}  // namespace

extern "C" {

int wasm_host_init(const char* pluginDir) {
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
    if (len < 5 || std::strcmp(e->d_name + len - 5, ".wasm") != 0) continue;
    ++g_count;
    if (chosen.empty()) chosen = std::string(pluginDir) + "/" + e->d_name;
  }
  ::closedir(d);
  if (chosen.empty()) { set_err("没有 .wasm 插件（正常：插件是可选的）"); return -3; }
  return wasm_plugin_load(chosen.c_str());
}

int wasm_host_reload(const char* pluginDir) {
  wasm_host_shutdown();
  return wasm_host_init(pluginDir);
}

int wasm_plugin_load(const char* path) {
  g_ready = false;
  FILE* f = std::fopen(path, "rb");
  if (!f) { set_err("插件打不开"); return -1; }
  std::fseek(f, 0, SEEK_END);
  const long size = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  if (size <= 8 || size > 1024 * 1024) { std::fclose(f); set_err("插件大小异常"); return -2; }
  std::vector<uint8_t> buf((size_t)size);
  if (std::fread(buf.data(), 1, buf.size(), f) != buf.size()) { std::fclose(f); set_err("插件读不满"); return -3; }
  std::fclose(f);
  if (!parse_module(buf.data(), buf.size())) return -4;

  // 调 abi()：必须回 1，否则拒载（版本不一致就别跑，别赌）
  if (g_mod.entryAbi != 0xFFFFFFFFu) {
    Val r;
    if (!call_entry(g_mod.entryAbi, {}, &r) || r.type != 0) { set_err("abi() 执行失败"); return -5; }
    if (r.i32 != 1) { set_err("插件 ABI 版本不是 1"); return -6; }
  }
  g_ready = true;
  return 0;
}

int wasm_plugin_process(float* interleaved, int frames) {
  if (!g_ready || !interleaved || frames <= 0) return -1;
  if (g_mod.entryNext != 0xFFFFFFFFu) {
    // 主流路径：逐点调 next() 拿增益（guest 里没有循环/分支，最不容易出错）
    for (int i = 0; i < frames; ++i) {
      Val r;
      if (!call_entry(g_mod.entryNext, {}, &r)) return -4;
      if (r.type != 1) { set_err("next() 没返回 f32"); return -5; }
      float g = r.f32;
      if (g > 2.0f) g = 2.0f;
      if (g < 0.0f) g = 0.0f;
      interleaved[i * 2] *= g;
      interleaved[i * 2 + 1] *= g;
    }
    return 0;
  }
  // 兼容路径：process(ptr, frames) 直接在宿主线性内存上改
  const size_t need = (size_t)frames * 2 * sizeof(float);
  const uint32_t base = 1024;
  if ((size_t)base + need > g_mod.memory.size()) return -2;
  std::memcpy(g_mod.memory.data() + base, interleaved, need);
  Val a1, a2, r;
  a1.type = 0; a1.i32 = base;
  a2.type = 0; a2.i32 = (uint32_t)frames;
  if (!call_entry(g_mod.entryProcess, {a1, a2}, &r)) return -4;
  if (r.type != 0 || r.i32 != 0) return -5;
  std::memcpy(interleaved, g_mod.memory.data() + base, need);
  return 0;
}

int wasm_plugin_ready(void) { return g_ready ? 1 : 0; }
int wasm_plugin_count(void) { return g_count; }
const char* wasm_host_error(void) { return g_err; }

void wasm_host_shutdown(void) {
  g_mod = Module();
  g_ready = false;
  g_count = 0;
}

}  // extern "C"
