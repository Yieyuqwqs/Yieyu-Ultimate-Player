#!/usr/bin/env node
/** 工具链体检：缺什么当场喊，别等 30 分钟后在链接阶段炸。 */
import { spawnSync } from 'node:child_process'
import fs from 'node:fs'
import path from 'node:path'

const soft = process.argv.includes('--soft')
const NDK = process.env.ANDROID_NDK || process.env.ANDROID_NDK_HOME || ''
const API = process.env.ANDROID_API || '26'
const c = { g: '\x1b[32m', y: '\x1b[33m', r: '\x1b[31m', d: '\x1b[2m', x: '\x1b[0m' }

const ver = (bin, args = ['--version']) => {
  const r = spawnSync(bin, args, { encoding: 'utf8', timeout: 20000 })
  if (r.error || r.status !== 0 && !r.stdout) return null
  return (r.stdout || r.stderr || '').trim().split('\n')[0].slice(0, 60)
}

const rows = []
const need = (mod, bin, args, note) => {
  const v = ver(bin, args)
  rows.push({ mod, bin, ok: !!v, v: v || '缺失', note })
}
need('4 / 12', 'zig', ['version'], 'zig cc 与 aarch64 交叉')
need('3 / 10 / 19', 'cargo', ['--version'], 'Rust 三件套')
need('3 / 10 / 19', 'rustup', ['--version'], '装 android target')
need('5', 'go', ['version'], '>=1.22')
need('1 / 2', 'flutter', ['--version'], 'Impeller 默认开')
need('6', 'node', ['-v'], '>=18，构建期唯一脚本运行时')
need('6', 'npm', ['-v'], 'ts-build 用')
need('21', 'flatc', ['--version'], 'FlatBuffers 编译器')
need('13 / 14 / 15 / 16 / …', 'cmake', ['--version'], '>=3.24')
need('13 / 14 / 15', 'ninja', ['--version'], '生成器')
need('22', 'java', ['-version'], 'JDK 17')
need('22', 'gradle', ['--version'], '或 ./gradlew')
need('11', 'clang', ['--version'], 'NDK 自带；O-LLVM 阶段需自建')

const ndkOk = NDK && fs.existsSync(NDK)
const cc = ndkOk ? path.join(NDK, 'toolchains/llvm/prebuilt/linux-x86_64/bin', `aarch64-linux-android${API}-clang`) : ''
rows.push({ mod: '全部', bin: 'ANDROID_NDK', ok: !!ndkOk, v: NDK || '未设置', note: 'r27c' })
rows.push({ mod: '26+', bin: 'android-api', ok: Number(API) >= 26, v: API, note: 'AAudio 门槛 26' })
if (cc) rows.push({ mod: '26+', bin: 'aarch64-clang', ok: fs.existsSync(cc), v: cc, note: '交叉编译器' })

const w = (s, n) => String(s).padEnd(n)
console.log(`\n${c.d}Yieyu-Ultimate-Player · 工具链体检${c.x}\n`)
console.log(`${c.d}${w('模块', 14)}${w('工具', 16)}${w('状态', 8)}${w('版本/路径', 30)}备注${c.x}`)
for (const r of rows) {
  const st = r.ok ? `${c.g}OK${c.x}` : `${c.r}MISS${c.x}`
  console.log(`${w(r.mod, 14)}${w(r.bin, 16)}${w(st, 8 + 9)}${w(r.v, 30)}${c.d}${r.note}${c.x}`)
}
const bad = rows.filter(r => !r.ok)
console.log(`\n${bad.length ? c.y + bad.length + ' 项缺失' + c.x : c.g + '全部就绪' + c.x}`)
if (bad.length) {
  console.log(`${c.d}缺失项先补：zig(0.13) / rustup target add aarch64-linux-android / flatc / NDK r27c${c.x}`)
  console.log(`${c.d}补不齐也能跑：orchestrate 会把缺工具的步骤 SKIP，其它步骤照常。${c.x}\n`)
  if (!soft) process.exit(1)
}
console.log()
