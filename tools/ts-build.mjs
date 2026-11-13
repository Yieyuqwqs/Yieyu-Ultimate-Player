#!/usr/bin/env node
/**
 * 模块6：不需要编译了。
 *
 * 依赖纪律：**零依赖**——没有 npm install、没有 tsc、没有 esbuild、没有 zod/axios。
 * 源码就是普通 ESM（ts/src/*.mjs），Node 直接 import；这里只做两件事：
 *   1) 把产物入口落到 out/ts/cleaner.mjs（构建期脚本用它）
 *   2) 跑一遍脏样本回归（洗不动就直接红）
 */
import fs from 'node:fs'
import path from 'node:path'
import { pathToFileURL, fileURLToPath } from 'node:url'

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..')
const OUT = path.resolve(ROOT, process.env.OUT || 'out')
const shim = path.join(OUT, 'ts', 'cleaner.mjs')
fs.mkdirSync(path.dirname(shim), { recursive: true })
fs.writeFileSync(shim, `// 模块6 产物入口（零依赖，直接指向源码）\nexport * from '${pathToFileURL(path.join(ROOT, 'ts/src/clean/index.mjs')).href}'\nexport { SONG_FIELDS } from '${pathToFileURL(path.join(ROOT, 'ts/src/schema/fields.mjs')).href}'\n`)
console.log(`  → ${path.relative(ROOT, shim)}`)

const mod = await import(pathToFileURL(path.join(ROOT, 'ts/src/clean/index.mjs')).href)
const dirty = JSON.stringify({ songs: [
  { id: '1', mid: 'm1', name: '晴天', artist: '周杰伦', album: '叶惠美' },
  { id: '', name: '' },
  { id: 2, name: 3, artist: null },
  'not-an-object',
] })
const out = mod.cleanSourceTrace({ source: 'wy', body: dirty })
if (!Array.isArray(out) || out.length === 0) { console.error('✗ 清洗回归失败'); process.exit(1) }
if (out.some((s) => !s || typeof s !== 'object')) { console.error('✗ 清洗结果混进非对象'); process.exit(1) }
console.log(`  ✓ 清洗回归：脏样本 4 条 → 干净 ${out.length} 条（零依赖 ESM）`)
