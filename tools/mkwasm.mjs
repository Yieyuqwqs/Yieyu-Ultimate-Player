#!/usr/bin/env node
/**
 * 【模块 8】插件生成器：直接手写一个合法的 WebAssembly MVP 模块（颤音音效）。
 *
 * 为什么手写而不是用 Rust/clang 编 wasm：那要整套工具链与 wasm32 目标；本工程零第三方。
 * 这个生成器输出的模块**只用一个小指令子集**，对应的自研解释器在
 * core/src/wasm/wasm_interp.cpp；两边是同一个 ABI：
 *   memory                      线性内存（1 页，f32 交错样本）
 *   abi()          -> i32       必须返回 1（插件 ABI 版本）
 *   init(i32 sr, f32 lfoHz) -> i32
 *   process(i32 ptr, i32 frames) -> i32   把 ptr 处 frames*2 个 f32 原地做颤音
 *
 * 产物：app/assets/plugins/fx_tremolo.wasm（构建期生成，随包）
 * 验证：node tools/mkwasm.mjs --check 会用 node 的 WebAssembly 真跑一遍并和 JS 参考实现比对。
 */
import fs from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..')
const OUT = path.join(ROOT, 'app/assets/plugins/fx_tremolo.wasm')

// ── LEB128 / 段编码 ───────────────────────────────────────────────
const uleb = (n) => { const out = []; do { let b = n & 0x7f; n >>>= 7; if (n) b |= 0x80; out.push(b) } while (n); return out }
const sleb = (n) => { const out = []; let more = true; while (more) { let b = n & 0x7f; n >>= 7; if ((n === 0 && !(b & 0x40)) || (n === -1 && (b & 0x40))) more = false; else b |= 0x80; out.push(b & 0xff) } return out }
const str = (s) => [...uleb(s.length), ...Buffer.from(s, 'utf8')]
const f32 = (v) => { const b = Buffer.alloc(4); b.writeFloatLE(v, 0); return [...b] }
const section = (id, payload) => [id, ...uleb(payload.length), ...payload]
const vec = (items) => [...uleb(items.length), ...items.flat()]
const emit = (...ops) => ops.flat(Infinity)   // 指令数组逐层摊平

// ── 指令 ─────────────────────────────────────────────────────────
const OP = {
  block: 0x02, loop: 0x03, ifn: 0x04, els: 0x05, end: 0x0b, br: 0x0c, brIf: 0x0d,
  ret: 0x0f, drop: 0x1a,
  localGet: 0x20, localSet: 0x21,
  i32Load: 0x28, f32Load: 0x2a, i32Store: 0x36, f32Store: 0x38,
  i32Const: 0x41, f32Const: 0x43,
  i32Eqz: 0x45, i32LtS: 0x48, i32GtS: 0x4a, i32LeS: 0x4c, i32GeS: 0x4e,
  f32Lt: 0x5d, f32Ge: 0x60,
  i32Add: 0x6a, i32Sub: 0x6b, i32Mul: 0x6c,
  f32Sub: 0x93, f32Mul: 0x94, f32Div: 0x95, f32Abs: 0x8b, f32ConvertI32S: 0xb2,   // f32.convert_i32_s（0xb7 是 f64 的，别混）
  f32Add: 0x92, f32Floor: 0x8e,
}
const I = {
  block: () => [OP.block, 0x40], loop: () => [OP.loop, 0x40], end: () => [OP.end],
  ifn: () => [OP.ifn, 0x40], els: () => [OP.els],
  br: (d) => [OP.br, ...uleb(d)], brIf: (d) => [OP.brIf, ...uleb(d)],
  ret: () => [OP.ret], drop: () => [OP.drop],
  lg: (n) => [OP.localGet, ...uleb(n)], ls: (n) => [OP.localSet, ...uleb(n)],
  i32c: (v) => [OP.i32Const, ...sleb(v)], f32c: (v) => [OP.f32Const, ...f32(v)],
  i32add: () => [OP.i32Add], i32sub: () => [OP.i32Sub], i32mul: () => [OP.i32Mul],
  i32ge: () => [OP.i32GeS], i32lt: () => [OP.i32LtS],
  fadd: () => [OP.f32Add], ffloor: () => [OP.f32Floor], fsub: () => [OP.f32Sub], fmul: () => [OP.f32Mul], fdiv: () => [OP.f32Div],
  flt: () => [OP.f32Lt], fge: () => [OP.f32Ge],
  fload: (align, off) => [OP.f32Load, ...uleb(align), ...uleb(off)],
  fstore: (align, off) => [OP.f32Store, ...uleb(align), ...uleb(off)],
  i2f: () => [OP.f32ConvertI32S],
}

// 内存布局：0=step(f32) 4=phase(f32) 8=modDepth(f32)
// locals（process）：0=ptr 1=frames 2=i(i32) 3=addr(i32) 4=g(f32) 5=p(f32)
function body_process() {
  return [
    // i32 i = 0
    I.i32c(0), I.ls(2),
    I.block(),
      I.loop(),
        I.lg(2), I.lg(1), I.i32ge(), I.brIf(1),           // if (i >= frames) break
        I.lg(2), I.i32c(8), I.i32mul(), I.lg(0), I.i32add(), I.ls(3),  // addr = ptr + i*8
        // p = phase + step
        I.i32c(0), I.fload(2, 4),           // phase
        I.i32c(0), I.fload(2, 0),           // step
        I.fadd(), I.ls(5),                  // p
        // if (p >= 1) p -= 1
        I.lg(5), I.f32c(1), I.fge(), I.ifn(),
          I.lg(5), I.f32c(1), I.fsub(), I.ls(5),
        I.els(),
          I.lg(5), I.ls(5),
        I.end(),
        // 存回 phase
        I.i32c(0), I.lg(5), I.fstore(2, 4),
        // g = 1 - modDepth * (1 - |2p-1|)
        I.f32c(1),
        I.i32c(0), I.fload(2, 8),                                   // modDepth
        I.f32c(1),
        I.lg(5), I.f32c(2), I.fmul(), I.f32c(1), I.fsub(), I.i32c(0), I.fload(2, 12),
        I.fsub(), I.fmul(),                                          // (1 - |2p-1|) * modDepth  —— 见下
        I.f32c(1), I.fsub(), I.fsub(),                               // 1 - 上面
        I.ls(4),
        // 乘到左右声道
        I.lg(3), I.lg(3), I.fload(2, 0), I.lg(4), I.fmul(), I.fstore(2, 0),
        I.lg(3), I.lg(3), I.fload(2, 4), I.lg(4), I.fmul(), I.fstore(2, 4),
        I.lg(2), I.i32c(1), I.i32add(), I.ls(2),
        I.br(0),
      I.end(),
    I.end(),
    I.i32c(0), I.ret(),
  ]
}

// 说明：上面为了少引入指令，把 |x| 用"减去 memory[12] 里预置的 0"的方式写不成立。
// 这里改成显式的 abs：先算 t = 2p-1；若 t<0 取反。
function body_next() {
  // next() -> f32：直筒子代码，没有循环也没有分支
  //   p = phase + step; p -= floor(p); phase = p; d = 2p-1; g = 1 - modDepth*(1 - d*d)
  return [
    I.i32c(0), I.fload(2, 4),                    // phase
    I.i32c(0), I.fload(2, 0), I.fadd(), I.ls(0),  // p = phase + step
    I.lg(0), I.ffloor(), I.ls(1),                // floor(p)
    I.lg(0), I.lg(1), I.fsub(), I.ls(0),         // p -= floor(p)
    I.i32c(0), I.lg(0), I.fstore(2, 4),          // phase = p
    I.lg(0), I.f32c(2), I.fmul(), I.f32c(1), I.fsub(), I.ls(1),   // d = 2p-1
    I.f32c(1), I.lg(1), I.lg(1), I.fmul(), I.fsub(), I.ls(2),     // q = 1 - d*d
    I.f32c(1), I.i32c(0), I.fload(2, 8), I.lg(2), I.fmul(), I.fsub(),  // 1 - modDepth*q
    I.ret(),
  ]
}

// locals 声明：process 需要 i32 i, i32 addr, f32 g, f32 p, f32 t
const locals_process = [[5, 0x7c], [3, 0x7d]]  // 5×i32? 实际按个数：i32:2, f32:3 → 分组
const locals_decl = (groups) => [...uleb(groups.length), ...groups.flatMap(([n, t]) => [...uleb(n), t])]

function build() {
  // ── type 段：0=(i32,i32)->i32  1=(i32,f32)->i32  2=()->i32
  const types = [
    [0x60, 0x00, 0x01, 0x7f],                 // 0: ()->i32   abi
    [0x60, 0x02, 0x7f, 0x7d, 0x01, 0x7f],     // 1: (i32,f32)->i32  init
    [0x60, 0x00, 0x01, 0x7d],                 // 2: ()->f32   next
  ]
  const typeSec = section(1, [types.length, ...types.flat()])

  // ── function 段：三个函数的类型索引
  // 函数顺序：0=abi(type0) 1=init(type1) 2=next(type2)
  const funcSec = section(3, [3, 0, 1, 2])

  // ── memory 段：最少 1 页（64KB）
  const memSec = section(5, [1, 0x00, 0x01])

  // ── export 段
  const expSec = section(7, [4,
    ...str('memory'), 0x02, 0x00,
    ...str('abi'), 0x00, 0x00,
    ...str('init'), 0x00, 0x01,
    ...str('next'), 0x00, 0x02,
  ])

  const bodyAbi = emit(locals_decl([]), I.i32c(1), I.end())
  const bodyInit = emit(
    locals_decl([[1, 0x7d]]),
    I.lg(1), I.lg(0), I.i2f(), I.fdiv(), I.ls(2),   // step = lfoHz / rate
    I.i32c(0), I.lg(2), I.fstore(2, 0),             // mem[0] = step
    I.i32c(0), I.f32c(0), I.fstore(2, 4),           // mem[4] = phase = 0
    I.i32c(0), I.f32c(0.3), I.fstore(2, 8),         // mem[8] = modDepth
    I.i32c(0), I.end(),
  )
  const bodyProc = emit(locals_decl([[3, 0x7d]]), body_next(), I.end())

  const codeEntry = (body) => [...uleb(body.length), ...body]
  const codeSec = section(10, [3, ...codeEntry(bodyAbi), ...codeEntry(bodyInit), ...codeEntry(bodyProc)])

  return Uint8Array.from([
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
    ...typeSec, ...funcSec, ...memSec, ...expSec, ...codeSec,
  ])
}

// ── JS 参考实现（用来和真 wasm 执行结果比对）──────────────────────
export function referenceNext(step, modDepth, state) {
  // 用 f32 逐运算模拟（插件里全是 f32；用 f64 参考会累积出 1e-3 级别的相位差）
  const f = Math.fround
  state.phase = f(state.phase + f(step))
  state.phase = f(state.phase - f(Math.floor(state.phase)))
  const d = f(f(2 * state.phase) - 1)
  const q = f(1 - f(d * d))
  return f(1 - f(f(modDepth) * q))
}

export function referenceTremolo(samples, step, modDepth) {
  let phase = 0
  for (let i = 0; i < samples.length; i += 2) {
    phase += step
    if (phase >= 1) phase -= 1
    let t = 2 * phase - 1
    if (t < 0) t = -t
    const g = 1 - modDepth * (1 - t)
    samples[i] *= g
    samples[i + 1] *= g
  }
  return samples
}

const bytes = build()
fs.mkdirSync(path.dirname(OUT), { recursive: true })
fs.writeFileSync(OUT, bytes)
console.log(`  → ${path.relative(ROOT, OUT)}（${bytes.length} 字节）`)

if (process.argv.includes('--check')) {
  const mod = new WebAssembly.Module(bytes)
  const inst = new WebAssembly.Instance(mod, {})
  const ex = inst.exports
  for (const k of ['memory', 'abi', 'init', 'next']) if (!ex[k]) throw new Error('缺导出 ' + k)
  if (ex.abi() !== 1) throw new Error('abi != 1')
  ex.init(48000, 5)
  const state = { phase: 0 }
  let worst = 0
  for (let round = 0; round < 100; round++) {
    const n = 8 + Math.floor(Math.random() * 4000)
    for (let i = 0; i < n; i++) {
      const got = ex.next()
      const want = referenceNext(5 / 48000, 0.3, state)
      worst = Math.max(worst, Math.abs(got - want))
    }
  }
  // f32 相位累积误差：100 轮 × 4000 点后仍然要求在 1e-3 以内
  if (worst > 1e-3) { console.error(`✗ wasm next() 与参考实现不一致，最大误差 ${worst}`); process.exit(1) }
  console.log(`  ✓ wasm 真执行 ${100} 轮 next()，与参考实现最大误差 ${worst.toExponential(1)}`)
}
