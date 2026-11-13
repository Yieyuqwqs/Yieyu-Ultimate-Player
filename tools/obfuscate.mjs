#!/usr/bin/env node
/**
 * 【模块 11】链接后处理：符号收敛 + 符号表重命名 + 剥离 + 自校验摘要（防篡改的产出侧）。
 *
 * 为什么不是 Obfuscator-LLVM：那需要自建 LLVM 分支，NDK r27 起 GCC 前端已废弃，套不上。
 * 这里做的是"改一个字节就自曝"这一层，成本几乎为零、每次构建都跑：
 *   1) 把 C++ 命名空间符号（_ZN5yieyu*）整批改成随机短名 —— 反汇编出来全是 s_3fa1c2；
 *   2) strip --strip-unneeded —— 去掉 .comment / 局部符号 / 调试信息；
 *   3) 算最终 so 的 sha256 → app/assets/integrity/libyieyu_core.sha256
 *      （运行时 [11] 自校验拿它比对，比对走 [12] 的 NEON 常数时间汇编）；
 *   4) 复核导出面：少一个符号就红 —— 混淆不许把模块 24 的入口搞没。
 */
import fs from 'node:fs'
import path from 'node:path'
import crypto from 'node:crypto'
import { spawnSync } from 'node:child_process'
import { fileURLToPath } from 'node:url'

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..')
const OUT = path.resolve(ROOT, process.env.OUT || 'out')
const SO = path.join(OUT, 'native/arm64/libyieyu_core.so')
const REPORT_DIR = path.join(OUT, 'obfuscate')
const DIGEST_FILE = path.join(ROOT, 'app/assets/integrity/libyieyu_core.sha256')

const MUST_EXPORT = [
  'Yieyu_Init', 'Yieyu_Shutdown', 'Yieyu_Post', 'Yieyu_Poll', 'Yieyu_Snapshot_Get',
  'Yieyu_PlayUrl', 'Yieyu_Pause', 'Yieyu_Resume', 'Yieyu_Stop', 'Yieyu_SeekMs', 'Yieyu_SetVolume',
  'Yieyu_Search', 'Yieyu_PlaySong', 'Yieyu_ScriptCall', 'Yieyu_ImportScript',
  'Yieyu_IntegrityCheck', 'Yieyu_SelfDigest', 'Yieyu_KvCount', 'Yieyu_StoreCount',
  'Yieyu_SetRate', 'Yieyu_HttpProbe', 'gesture_start', 'gesture_poll', 'yieyu_mem_alive', 'Yieyu_MakeKaraoke', 'Yieyu_KaraokeStatus', 'Yieyu_ImportPlugin', 'Yieyu_SetPlugin',
]

function tcBin(name) {
  const ndk = process.env.ANDROID_NDK || process.env.ANDROID_NDK_HOME || ''
  const cands = ndk
    ? [path.join(ndk, 'toolchains/llvm/prebuilt/linux-x86_64/bin', name),
       path.join(ndk, 'toolchains/llvm/prebuilt/darwin-x86_64/bin', name)]
    : []
  for (const c of cands) if (fs.existsSync(c)) return c
  const r = spawnSync('sh', ['-c', `command -v ${name}`], { encoding: 'utf8' })
  return r.status === 0 ? r.stdout.trim() : ''
}

const run = (bin, args) => {
  const r = spawnSync(bin, args, { encoding: 'utf8' })
  return { ok: r.status === 0, out: (r.stdout || '') + (r.stderr || '') }
}

const exported = (nmBin) => {
  const r = run(nmBin, ['-D', '--defined-only', SO])
  const set = new Set()
  for (const line of r.out.split('\n')) {
    const m = line.trim().split(/\s+/)
    if (m.length >= 3) set.add(m[m.length - 1])
  }
  return set
}

function main() {
  if (!fs.existsSync(SO)) {
    console.error(`✗ 没有 ${SO}（先跑 ./build.sh native）`)
    process.exit(1)
  }
  fs.mkdirSync(REPORT_DIR, { recursive: true })

  const nm = tcBin('llvm-nm')
  const objcopy = tcBin('llvm-objcopy')
  const strip = tcBin('llvm-strip')
  const lines = []

  const before = nm ? exported(nm) : new Set()
  lines.push(`导出面（混淆前）：${before.size} 个`)

  // ① 符号表重命名（只动 yieyu 命名空间下的隐藏符号，导出面一律不碰）
  let renamed = 0
  if (nm && objcopy) {
    const r = run(nm, ['--defined-only', SO])
    const map = []
    let seed = 0
    for (const line of r.out.split('\n')) {
      const parts = line.trim().split(/\s+/)
      if (parts.length < 3) continue
      const sym = parts[parts.length - 1]
      if (!/^_ZN?5yieyu/.test(sym) && !/^_ZN?4yieyu/.test(sym)) continue
      if (before.has(sym)) continue            // 双保险：导出过的绝不动
      seed = (seed * 1103515245 + 12345) & 0x7fffffff
      map.push(`${sym} s_${seed.toString(16)}`)
    }
    if (map.length) {
      const mapFile = path.join(REPORT_DIR, 'rename.map')
      fs.writeFileSync(mapFile, map.join('\n') + '\n')
      const rc = run(objcopy, [`--redefine-syms=${mapFile}`, SO])
      if (rc.ok) {
        renamed = map.length
        lines.push(`符号重命名：${renamed} 个`)
      } else {
        lines.push(`符号重命名失败（不致命）：${rc.out.trim().slice(0, 160)}`)
      }
    }
  } else {
    lines.push('跳过符号重命名（没有 llvm-objcopy）')
  }

  // ② 剥离
  if (strip) {
    const rc = run(strip, ['--strip-unneeded', SO])
    lines.push(rc.ok ? 'strip --strip-unneeded 完成' : `strip 失败（不致命）：${rc.out.trim().slice(0, 120)}`)
  }

  // ③ 导出面复核（混淆不许把入口搞没）
  if (nm) {
    const after = exported(nm)
    const missing = MUST_EXPORT.filter((s) => !after.has(s))
    if (missing.length) {
      console.error(`✗ 混淆后缺导出：${missing.join(', ')}`)
      process.exit(1)
    }
    lines.push(`导出面复核：${after.size} 个，必需符号齐全`)
  }

  // ④ 摘要 → APK 资产（运行时 [11] 用它自校验）
  const buf = fs.readFileSync(SO)
  const hex = crypto.createHash('sha256').update(buf).digest('hex')
  fs.mkdirSync(path.dirname(DIGEST_FILE), { recursive: true })
  fs.writeFileSync(DIGEST_FILE, hex + '\n')
  lines.push(`sha256 = ${hex}`)
  lines.push(`体积 = ${(buf.length / 1024 / 1024).toFixed(2)} MB`)

  fs.writeFileSync(path.join(REPORT_DIR, 'report.md'),
    `# 模块11 链接后处理报告\n\n- 输入：\`${path.relative(ROOT, SO)}\`\n${lines.map((l) => '- ' + l).join('\n')}\n`)
  console.log(lines.map((l) => '  ' + l).join('\n'))
}

main()
