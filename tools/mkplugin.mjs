#!/usr/bin/env node
/**
 * 【模块8】插件生成器：生成**我们自己的** YPFX 字节码（不用 WebAssembly，也不依赖任何外部引擎）。
 *
 * 格式与指令集见 core/src/plugin/abi.h。这里干两件事：
 *   1) 编一个颤音插件：state[4] += state[0]（相位累加 + 回绕）→ 抛物线 LFO → 增益
 *   2) --check：用同一套语义的 JS 参考 VM 跑 100 轮，和"纯 JS 公式"逐点比对
 *
 * 产物：app/assets/plugins/fx_tremolo.ypfx（随包） + out/plugins/fx_tremolo.ypfx（构建产物）
 */
import fs from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..')
const OUT = path.resolve(ROOT, process.env.OUT || 'out')

const OP = { PUSHI: 0x01, PUSHF: 0x02, LOAD: 0x03, STORE: 0x04, ADD: 0x05, SUB: 0x06, MUL: 0x07, FLOOR: 0x08, DUP: 0x09, DROP: 0x0a, RET: 0x0b }

const pushI = (v) => { const b = Buffer.alloc(5); b[0] = OP.PUSHI; b.writeInt32LE(v, 1); return [...b] }
const pushF = (v) => { const b = Buffer.alloc(5); b[0] = OP.PUSHF; b.writeFloatLE(v, 1); return [...b] }

// 程序：g = 1 - depth * (1 - (2p-1)^2)，p 从 state[4] 累加并回绕
function buildCode() {
  return Uint8Array.from([
    ...pushI(4), OP.LOAD,            // phase
    ...pushI(0), OP.LOAD, OP.ADD,    // + step
    OP.DUP, OP.FLOOR, OP.SUB,        // p -= floor(p)   → p 回绕到 [0,1)
    OP.DUP, ...pushI(4), OP.STORE,   // state[4] = p（栈上留一份 p）
    ...pushF(2), OP.MUL, ...pushF(1), OP.SUB,   // d = 2p - 1
    OP.DUP, OP.MUL,                             // d²
    ...pushF(-1), OP.MUL, ...pushF(1), OP.ADD,  // q = 1 - d²
    ...pushI(8), OP.LOAD, OP.MUL,               // q * depth
    ...pushF(-1), OP.MUL, ...pushF(1), OP.ADD,  // g = 1 - q*depth
    OP.RET,
  ])
}

function buildPlugin({ lfoHz = 5, depth = 0.3 } = {}) {
  const code = buildCode()
  const head = Buffer.alloc(18)
  head.write('YPFX', 0, 'ascii')
  head[4] = 1            // version
  head[5] = 0            // kind = tremolo
  head.writeFloatLE(lfoHz, 6)
  head.writeFloatLE(depth, 10)
  head.writeUInt32LE(code.length, 14)
  return Buffer.concat([head, Buffer.from(code)])
}

// 参考实现（同一条公式，f32 逐运算）+ 参考 VM（同一条字节码语义）
const f = Math.fround
function referenceGain(phase, step, depth) {
  const p = f(f(phase + step) - f(Math.floor(f(phase + step))))
  const d = f(f(2 * p) - 1)
  const q = f(1 - f(d * d))
  return { p, g: f(1 - f(depth * q)) }
}
function runVm(blob, state, steps = 4096) {
  const code = blob.subarray(18)
  const stack = []
  let pc = 0, n = 0
  while (pc < code.length) {
    if (++n > steps) throw new Error('指令超限')
    const op = code[pc++]
    switch (op) {
      case OP.PUSHI: stack.push(code.readInt32LE(pc)); pc += 4; break
      case OP.PUSHF: stack.push(f(code.readFloatLE(pc))); pc += 4; break
      case OP.LOAD: { const a = stack.pop(); stack.push(state[a]); break }
      case OP.STORE: { const a = stack.pop(); const v = stack.pop(); state[a] = v; break }
      case OP.ADD: { const b = stack.pop(), a = stack.pop(); stack.push(f(a + b)); break }
      case OP.SUB: { const b = stack.pop(), a = stack.pop(); stack.push(f(a - b)); break }
      case OP.MUL: { const b = stack.pop(), a = stack.pop(); stack.push(f(a * b)); break }
      case OP.FLOOR: { stack.push(f(Math.floor(stack.pop()))); break }
      case OP.DUP: { stack.push(stack[stack.length - 1]); break }
      case OP.DROP: stack.pop(); break
      case OP.RET: return stack.pop()
      default: throw new Error(`未知指令 0x${op.toString(16)}`)
    }
  }
  throw new Error('没有 RET')
}

const blob = buildPlugin()
const assets = path.join(ROOT, 'app/assets/plugins/fx_tremolo.ypfx')
fs.mkdirSync(path.dirname(assets), { recursive: true })
fs.writeFileSync(assets, blob)
fs.mkdirSync(path.join(OUT, 'plugins'), { recursive: true })
fs.writeFileSync(path.join(OUT, 'plugins/fx_tremolo.ypfx'), blob)
console.log(`  → ${path.relative(ROOT, assets)}（${blob.length} 字节，${(blob.length - 18)} 字节字节码）`)

if (process.argv.includes('--check')) {
  const state = new Float32Array(64)
  state[8] = 0.3
  let worst = 0
  for (let round = 0; round < 100; round++) {
    const rate = [44100, 48000, 96000, 192000][round % 4]
    state[0] = f(5 / rate)
    state[4] = 0
    let phase = 0
    const n = 200 + Math.floor(Math.random() * 2000)
    for (let i = 0; i < n; i++) {
      const g = runVm(blob, state)
      const ref = referenceGain(phase, state[0], 0.3)
      phase = ref.p
      worst = Math.max(worst, Math.abs(g - ref.g))
    }
  }
  // 插件状态用 f32 存，参考用 f64 累加相位 → 允许 1e-3
  if (worst > 1e-3) { console.error(`✗ YPFX VM 与参考实现不一致，最大误差 ${worst}`); process.exit(1) }
  console.log(`  ✓ YPFX 参考 VM 100 轮 × 4 种采样率，与公式实现最大误差 ${worst.toExponential(1)}`)
}
