#!/usr/bin/env node
/**
 * 本机（没有 python3）用的等价生成器：与 python/vocal_remover/design.py 逐值同源。
 * 只在"要刷新入树头文件"时手动跑；CI 的真源始终是 design.py（check-ai-kernel.mjs 会比对）。
 */
import fs from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..')
const NFFT = 2048
const half = NFFT / 2 + 1
const hann = (n, total) => 0.5 * (1 - Math.cos((2 * Math.PI * n) / total))
const centerWeight = (f) => {
  if (f < 80 || f > 8000) return 0
  if (f < 200) return 0.5 * (1 - Math.cos((Math.PI * (f - 80)) / 120))
  if (f > 5000) return 0.5 * (1 + Math.cos((Math.PI * (f - 5000)) / 3000))
  return 1
}
const fmt = (v) => v.toFixed(9).replace(/0+$/, '').replace(/\.$/, '.0') + 'f'

const win = Array.from({ length: NFFT }, (_, i) => hann(i, NFFT))
const w = Array.from({ length: half }, (_, i) => centerWeight((1000 * i) / half))

const lines = []
lines.push('// 由 python/vocal_remover/design.py 生成，**不要手改**。')
lines.push('// CI 里 tools/check-ai-kernel.mjs 会重新跑一次生成器并逐值比对（差 1e-6 就红）。')
lines.push('#pragma once')
lines.push('')
lines.push('namespace yieyu {')
lines.push('namespace vocal {')
lines.push(`constexpr int   kNfft = ${NFFT};`)
lines.push(`constexpr int   kHop = ${NFFT / 4};`)
lines.push(`constexpr int   kBins = ${half};`)
lines.push('constexpr float kMaskPower = 2.0f;      // |mid|^p / (|mid|^p + |side|^p)')
lines.push('constexpr float kCenterAtten = 0.95f;   // 中置最多压掉 95%（留一点，避免"挖空"）')
lines.push('constexpr float kEps = 1e-9f;')
lines.push('')
lines.push('// 周期 Hann 分析/合成窗（平方后满足 COLA，hop = N/4）')
lines.push('static const float kWindow[kNfft] = {')
for (let i = 0; i < NFFT; i += 8) lines.push('    ' + win.slice(i, i + 8).map(fmt).join(', ') + ',')
lines.push('};')
lines.push('')
lines.push('// 中置权重曲线（按 1kHz 尺度索引，运行期按实际 bin 频率线性取样）')
lines.push('static const float kWeightShape[kBins] = {')
for (let i = 0; i < half; i += 8) lines.push('    ' + w.slice(i, i + 8).map(fmt).join(', ') + ',')
lines.push('};')
lines.push('}  // namespace vocal')
lines.push('}  // namespace yieyu')
lines.push('')

const out = path.join(ROOT, 'core/src/ai/vocal_kernel.h')
fs.mkdirSync(path.dirname(out), { recursive: true })
fs.writeFileSync(out, lines.join('\n'))
console.log(`  → ${path.relative(ROOT, out)}（${NFFT} + ${half} 个系数）`)
