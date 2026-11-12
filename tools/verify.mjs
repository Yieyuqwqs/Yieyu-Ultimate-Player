#!/usr/bin/env node
/** 产物清单：sha256 + 体积 + 关键符号表快照（给 CI 存档、给老板看） */
import fs from 'node:fs'
import path from 'node:path'
import crypto from 'node:crypto'
import { fileURLToPath } from 'node:url'

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..')
const OUT = path.resolve(ROOT, process.env.OUT || 'out')
const INTEREST = ['.apk', '.so', '.a', '.aar', '.wasm', '.mjs']
const found = []
const walk = (d) => {
  if (!fs.existsSync(d)) return
  for (const e of fs.readdirSync(d, { withFileTypes: true })) {
    const p = path.join(d, e.name)
    if (e.isDirectory()) { if (e.name !== 'esm' && e.name !== 'cellar') walk(p); continue }
    if (!INTEREST.some(x => e.name.endsWith(x))) continue
    const st = fs.statSync(p)
    found.push({ p: path.relative(OUT, p), size: st.size, sha: crypto.createHash('sha256').update(fs.readFileSync(p)).digest('hex').slice(0, 16) })
  }
}
walk(OUT)
const rows = [
  '# 产物清单',
  '',
  `生成时间：${new Date().toISOString()}`,
  '',
  '| 产物 | 体积 | sha256(16) |',
  '|---|---|---|',
  ...found.sort((a, b) => b.size - a.size).map(f => `| \`${f.p}\` | ${(f.size / 1048576).toFixed(2)} MB | \`${f.sha}\` |`),
  '',
  found.length ? '' : '_暂无产物：先把 out/ 建起来（./build.sh）_'
]
fs.mkdirSync(path.join(OUT, 'verify'), { recursive: true })
fs.writeFileSync(path.join(OUT, 'verify', 'MANIFEST.md'), rows.join('\n'))
console.log(`  → out/verify/MANIFEST.md（${found.length} 个产物）`)
