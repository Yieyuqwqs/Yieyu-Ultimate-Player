#!/usr/bin/env node
/**
 * 【模块 18】核系数一致性闸门：
 *   python/vocal_remover/design.py 是唯一真源；core/src/ai/vocal_kernel.h 是它的产物。
 *   每次构建重跑生成器，把结果和入树的头文件逐值比对（容差 1e-6），
 *   有人手改头文件、或者改了生成器忘了重新生成，这里都会红。
 *
 * 没有 python3 的机器（比如本机）只做语法存在性检查，不阻塞。
 */
import fs from 'node:fs'
import path from 'node:path'
import { spawnSync } from 'node:child_process'
import { fileURLToPath } from 'node:url'

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..')
const GEN = 'python/vocal_remover/design.py'
const HDR = path.join(ROOT, 'core/src/ai/vocal_kernel.h')

if (!fs.existsSync(HDR)) { console.error(`✗ 缺 ${path.relative(ROOT, HDR)}（先跑 python3 ${GEN}）`); process.exit(1) }

const py = spawnSync('python3', ['--version'], { encoding: 'utf8' })
if (py.status !== 0) {
  console.log('  · 没有 python3，跳过系数比对（只检查头文件存在）')
  process.exit(0)
}

// 备份 → 重新生成 → 比对 → 还原
const backup = fs.readFileSync(HDR, 'utf8')
const r = spawnSync('python3', [GEN], { encoding: 'utf8' })
if (r.status !== 0) { console.error('✗ 生成器失败：' + (r.stderr || r.stdout)); process.exit(1) }
const fresh = fs.readFileSync(HDR, 'utf8')
fs.writeFileSync(HDR, backup)   // 构建不该改源码树：比对完还原

const nums = (s) => (s.match(/-?\d+\.\d+(?:e-?\d+)?f/g) || []).map((x) => parseFloat(x))
const a = nums(backup), b = nums(fresh)
if (a.length !== b.length) { console.error(`✗ 系数个数不一致：入树 ${a.length} / 生成器 ${b.length}`); process.exit(1) }
let worst = 0, at = -1
for (let i = 0; i < a.length; i++) {
  const d = Math.abs(a[i] - b[i])
  if (d > worst) { worst = d; at = i }
}
if (worst > 1e-6) { console.error(`✗ 系数不一致：第 ${at} 个差 ${worst}（>1e-6）→ 重新跑 python3 ${GEN}`); process.exit(1) }
console.log(`  ✓ 模块18 核系数一致（${a.length} 个，最大误差 ${worst.toExponential(1)}）`)
