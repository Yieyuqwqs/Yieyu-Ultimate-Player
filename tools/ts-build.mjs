#!/usr/bin/env node
/** 模块6：TS → 单个 cleaner.mjs。产物只进 $OUT/ts，不污染源码树。 */
import { spawnSync } from 'node:child_process'
import path from 'node:path'
import fs from 'node:fs'
import { fileURLToPath } from 'node:url'

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..')
const OUT = path.resolve(ROOT, process.env.OUT || 'out')
const run = (bin, args, cwd) => {
  console.log('  $', bin, args.join(' '))
  const r = spawnSync(bin, args, { cwd, stdio: 'inherit' })
  if (r.status !== 0) { console.error(`失败: ${bin} ${args.join(' ')}`); process.exit(r.status || 1) }
}
const ts = path.join(ROOT, 'ts')
const esmOut = path.join(OUT, 'ts', 'esm')
fs.mkdirSync(esmOut, { recursive: true })

if (!fs.existsSync(path.join(ts, 'node_modules'))) run('npm', ['ci', '--no-audit', '--no-fund'], ts)
run(path.join(ts, 'node_modules/.bin/tsc'), ['-p', 'tsconfig.json', '--outDir', esmOut], ts)

const entry = path.join(esmOut, 'clean', 'index.js')
const bundle = path.join(OUT, 'ts', 'cleaner.mjs')
const r = spawnSync(path.join(ts, 'node_modules/.bin/esbuild'), [entry, '--bundle', '--format=esm', '--minify', `--outfile=${bundle}`], { stdio: 'inherit' })
if (r.status !== 0) { console.error('esbuild 失败 → 若尚未 npm i esbuild，先 npx esbuild'); process.exit(1) }
const kb = (fs.statSync(bundle).size / 1024).toFixed(1)
console.log(`  → ${bundle} (${kb} KB)`)
