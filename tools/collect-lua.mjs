#!/usr/bin/env node
/** 模块7：把 lua/ 下的音源脚本打进 $OUT/lua/，并做白名单静态检查（拒绝 os.execute/io.open）。 */
import fs from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..')
const OUT = path.resolve(ROOT, process.env.OUT || 'out')
const SRC = path.join(ROOT, 'lua')
const DST = path.join(OUT, 'lua')
const ASSET = path.join(ROOT, 'app/assets/scripts')   // 随包脚本：首启由 Dart 导入到脚本目录
const BAN = [/\bos\s*\.\s*execute/, /\bio\s*\.\s*open/, /\bos\s*\.\s*exit/, /\bpackage\s*\.\s*loadlib/, /\bdebug\s*\.\s*sethook/]

fs.mkdirSync(DST, { recursive: true })
fs.mkdirSync(ASSET, { recursive: true })
let n = 0, bad = 0
const walk = (d) => {
  for (const e of fs.readdirSync(d, { withFileTypes: true })) {
    const p = path.join(d, e.name)
    if (e.isDirectory()) { walk(p); continue }
    if (!e.name.endsWith('.lua')) continue
    const src = fs.readFileSync(p, 'utf8')
    const hits = BAN.filter(re => re.test(src))
    const rel = path.relative(SRC, p)
    if (hits.length) { console.error(`  ✗ 沙箱违规 ${rel}: ${hits.join(' ')}`); bad++; continue }
    fs.writeFileSync(path.join(DST, rel.replace(/[\/]/g, '__')), src)
    fs.writeFileSync(path.join(ASSET, path.basename(p)), src)   // 给 APK 用（保留 .lua 后缀，导入接口按名字校验）
    console.log(`  ✓ ${rel}`); n++
  }
}
walk(SRC)
console.log(`  → ${n} 个脚本入包（out/lua + app/assets/scripts），${bad} 个被拒`)
if (bad) process.exit(1)
