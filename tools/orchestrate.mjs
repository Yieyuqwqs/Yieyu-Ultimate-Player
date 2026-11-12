#!/usr/bin/env node
/**
 * Yieyu-Ultimate-Player 构建总控调度器
 * 零依赖（node >= 18）。CI 与本地同一条命令，不许有第二套脚本。
 *
 *   ./build.sh                        # 跑全部（缺工具链的步骤 SKIP 而不是 FAIL）
 *   ./build.sh doctor                 # 只体检
 *   ./build.sh zig rust idl native    # 只跑指定步骤（含其依赖）
 *   ./build.sh --dry-run              # 只打印将要执行的命令
 *   ./build.sh --clean native         # 只清某步产物
 */
import { execFileSync, spawnSync } from 'node:child_process'
import { fileURLToPath } from 'node:url'
import fs from 'node:fs'
import path from 'node:path'
import os from 'node:os'
import crypto from 'node:crypto'

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..')
const OUT = path.resolve(ROOT, process.env.OUT || 'out')
const NDK = process.env.ANDROID_NDK || process.env.ANDROID_NDK_HOME || ''
const API = process.env.ANDROID_API || '26'
const JOBS = Number(process.env.YIEYU_JOBS || Math.max(2, Math.min(4, os.cpus().length)))
const OFFLINE = (process.env.YIEYU_OFFLINE_ASSETS || 'ON') === 'ON'
const MANIFEST = path.join(OUT, '.build-manifest.json')

const TOOLCHAIN = path.join(NDK, 'toolchains/llvm/prebuilt/linux-x86_64/bin') // 也兼容 darwin
const toolchainCandidates = () => [
  path.join(NDK, 'toolchains/llvm/prebuilt/linux-x86_64/bin'),
  path.join(NDK, 'toolchains/llvm/prebuilt/darwin-x86_64/bin'),
]

const log = (s) => process.stdout.write(s + '\n')
const c = { g: '\x1b[32m', y: '\x1b[33m', r: '\x1b[31m', d: '\x1b[2m', x: '\x1b[0m' }
const tag = (s, col) => `${col}${s}${c.x}`.padEnd(24)

// ─────────────────────────── 步骤定义（DAG 唯一真源） ───────────────────────────
const STEPS = [
  { id: 'doctor', deps: [], bin: 'node', desc: '工具链体检', out: [] },

  { id: 'idl', deps: ['doctor'], bin: 'flatc', desc: '模块21 FlatBuffers 代码生成',
    cmd: () => ['flatc', '--cpp', '--dart', '--rust', '--go', '--gen-object-api', '-o', at('gen'), ...fbs()],
    out: ['gen'], inputs: ['idl'] },

  { id: 'zig', deps: ['doctor'], bin: 'zig', desc: '模块4 分配器静态库',
    cmd: () => ['zig', 'build', '-Dtarget=aarch64-linux-android', '-Doptimize=ReleaseFast', '--prefix', at('zig')],
    cwd: 'zig', out: ['zig'], inputs: ['zig'] },

  { id: 'rust', deps: ['doctor'], bin: 'cargo', desc: '模块3/10/19 三个 .so', cwd: 'rust',
    cmd: () => ['cargo', 'build', '--release',
      '--target', 'aarch64-linux-android'],
    env: () => androidEnv(), out: ['rust'], inputs: ['rust'] },

  { id: 'go', deps: ['doctor'], bin: 'gomobile', desc: '模块5 搜歌 AAR',
    cmd: () => ['bash', 'build-aar.sh'],
    cwd: 'go', env: () => androidEnv(), out: ['go'], inputs: ['go'] },

  { id: 'ts', deps: ['doctor'], bin: 'npm', desc: '模块6 TS 清洗层 → cleaner.mjs',
    cmd: () => ['node', 'tools/ts-build.mjs'], out: ['ts'], inputs: ['ts'] },

  { id: 'wasm', deps: ['doctor'], bin: 'cargo', desc: '模块8 插件 wasm32-wasip1',
    cmd: () => ['cargo', 'build', '--manifest-path', 'wasm/fx_tremolo/Cargo.toml', '--release',
      '--target', 'wasm32-wasip1'],
    out: ['wasm'], inputs: ['wasm'] },

  { id: 'lua', deps: ['doctor'], bin: 'node', desc: '模块7 音源脚本资源收集',
    cmd: () => ['node', 'tools/collect-lua.mjs'], out: ['lua'], inputs: ['lua'] },

  { id: 'native', deps: ['idl', 'zig', 'rust'], bin: 'cmake', desc: '模块7/8/9/12/13/14/15/16/17/18/20/21/23/24 → libyieyu_core.so',
    cmd: () => ['bash', 'core/build-native.sh'], env: () => ({ ...androidEnv(), YIEYU_OUT: OUT, YIEYU_OFFLINE_ASSETS: OFFLINE ? 'ON' : 'OFF' }),
    out: ['native'], inputs: ['core', 'idl'] },

  { id: 'gradle', deps: ['native'], bin: 'gradle', desc: '模块22 Kotlin 前台服务 AAR',
    cmd: () => ['gradle', '-p', 'android', ':platform-service:assembleRelease'], out: ['android'], inputs: ['android'] },

  { id: 'flutter', deps: ['native', 'go', 'ts', 'wasm', 'lua', 'gradle'], bin: 'flutter', desc: '模块1/2 出 APK',
    cmd: () => ['bash', 'app/build-app.sh'], env: () => ({ YIEYU_OUT: OUT }), out: ['apk'], inputs: ['app'] },

  { id: 'verify', deps: ['flutter'], bin: 'node', desc: '产物清单 + sha256 + 体积',
    cmd: () => ['node', 'tools/verify.mjs'], out: ['verify'] }
]

const at = (p) => path.join(OUT, p)
const fbs = () => {
  const d = path.join(ROOT, 'idl')
  if (!fs.existsSync(d)) return []
  return fs.readdirSync(d).filter(f => f.endsWith('.fbs')).map(f => path.join('idl', f))
}
function androidEnv() {
  const tc = toolchainCandidates().find(p => fs.existsSync(p)) || TOOLCHAIN
  const cc = path.join(tc, `aarch64-linux-android${API}-clang`)
  return {
    CC_aarch64_linux_android: cc, CXX_aarch64_linux_android: cc,
    AR_aarch64_linux_android: path.join(tc, 'llvm-ar'),
    CARGO_TARGET_AARCH64_LINUX_ANDROID_LINKER: cc,
    ANDROID_NDK: NDK, ANDROID_API: API, ANDROID_NDK_TOOLCHAIN_BIN: tc,
    PATH: `${tc}:${process.env.PATH}`,
    CGO_ENABLED: '1', GOOS: 'android', GOARCH: 'arm64', CC: cc
  }
}

// ─────────────────────────── 参数解析 ───────────────────────────
const argv = process.argv.slice(2)
const flags = new Set(argv.filter(a => a.startsWith('--')))
const clean = argv[0] === 'clean'
const words = argv.filter(a => !a.startsWith('--'))
if (clean) words.shift()
const wanted = (clean ? words : words).filter(w => w !== 'all')

const hasBin = (bin) => spawnSync('sh', ['-c', `command -v ${bin}`], { stdio: 'ignore' }).status === 0

// 依赖闭包
function closure(ids) {
  const set = new Set()
  const walk = (id) => {
    if (set.has(id)) return
    const s = STEPS.find(x => x.id === id)
    if (!s) { log(`${c.r}未知步骤: ${id}${c.x}`); process.exit(2) }
    set.add(id); s.deps.forEach(walk)
  }
  ;(ids.length ? ids : STEPS.map(s => s.id)).forEach(walk)
  return STEPS.filter(s => set.has(s.id))
}

// 拓扑分层
function levels(steps) {
  const byId = new Map(steps.map(s => [s.id, s]))
  const out = []
  const placed = new Set()
  while (placed.size < steps.length) {
    const layer = steps.filter(s => !placed.has(s.id) && s.deps.every(d => placed.has(d) || !byId.has(d)))
    if (!layer.length) throw new Error('DAG 有环：' + [...byId.keys()].filter(k => !placed.has(k)))
    layer.forEach(s => placed.add(s.id)); out.push(layer)
  }
  return out
}

// ─────────────────────────── 增量指纹 ───────────────────────────
function fingerprint(step, envMap) {
  const h = crypto.createHash('sha256')
  h.update(step.id).update(JSON.stringify(envMap || {}))
  h.update(String(step.cmd ? step.cmd() : 'builtin'))
  for (const rel of step.inputs || []) {
    const abs = path.join(ROOT, rel)
    if (!fs.existsSync(abs)) { h.update('missing:' + rel); continue }
    const stack = [abs]
    while (stack.length) {
      const p = stack.pop()
      const st = fs.statSync(p)
      if (st.isDirectory()) fs.readdirSync(p).forEach(f => stack.push(path.join(p, f)))
      else h.update(`${p}:${st.size}:${Math.floor(st.mtimeMs)}`)
    }
  }
  return h.digest('hex').slice(0, 16)
}
const readManifest = () => { try { return JSON.parse(fs.readFileSync(MANIFEST, 'utf8')) } catch { return {} } }
const writeManifest = (m) => { fs.mkdirSync(OUT, { recursive: true }); fs.writeFileSync(MANIFEST, JSON.stringify(m, null, 2)) }

// ─────────────────────────── 执行 ───────────────────────────
const results = []
function runStep(step, manifest) {
  if (step.id === 'doctor') {
    const r = spawnSync('node', [path.join(ROOT, 'tools/doctor.mjs'), '--soft'], { stdio: 'inherit' })
    results.push({ id: step.id, status: r.status === 0 ? 'OK' : 'WARN' }); return
  }
  const envMap = { ...process.env, ...(step.env ? step.env() : {}) }
  const fp = fingerprint(step, step.env ? step.env() : {})

  if (clean) {
    for (const o of step.out) fs.rmSync(at(o), { recursive: true, force: true })
    delete manifest[step.id]; writeManifest(manifest)
    log(`${tag('CLEAN', c.y)} ${step.id}`); results.push({ id: step.id, status: 'CLEAN' }); return
  }
  if (!hasBin(step.bin)) {
    log(`${tag('SKIP', c.d)} ${step.id}  ${c.d}缺 ${step.bin}${c.x}`)
    results.push({ id: step.id, status: 'SKIP', why: 'no ' + step.bin }); return
  }
  if (manifest[step.id] && manifest[step.id].fp === fp && step.out.every(o => fs.existsSync(at(o)))) {
    log(`${tag('SKIP', c.d)} ${step.id}  ${c.d}已是最新 ${fp}${c.x}`)
    results.push({ id: step.id, status: 'SKIP', why: 'up-to-date' }); return
  }
  const cmd = step.cmd ? step.cmd() : ['true']
  const cwd = step.cwd ? path.join(ROOT, step.cwd) : ROOT
  log(`${tag('RUN', c.g)} ${step.id}  ${c.d}${path.basename(cmd[0])} ${cmd.slice(1, 3).join(' ')}${c.x}`)
  if (flags.has('--dry-run')) { results.push({ id: step.id, status: 'DRY' }); return }

  const t0 = Date.now()
  const r = spawnSync(cmd[0], cmd.slice(1), { cwd, env: envMap, stdio: 'inherit' })
  const dt = ((Date.now() - t0) / 1000).toFixed(1)
  if (r.status !== 0) {
    log(`${tag('FAIL', c.r)} ${step.id}  exit=${r.status}  ${dt}s`)
    results.push({ id: step.id, status: 'FAIL', code: r.status })
    return
  }
  manifest[step.id] = { fp, at: new Date().toISOString(), sec: Number(dt) }
  writeManifest(manifest)
  log(`${tag('OK', c.g)} ${step.id}  ${c.d}${dt}s${c.x}`)
  results.push({ id: step.id, status: 'OK', sec: Number(dt) })
}

async function main() {
  const steps = closure(wanted)
  fs.mkdirSync(OUT, { recursive: true })
  log(`\n${c.d}Yieyu-Ultimate-Player · 构建总控${c.x}`)
  log(`${c.d}root=${ROOT}  out=${OUT}  ndk=${NDK || '(未设)'}  api=${API}  jobs=${JOBS}  offline=${OFFLINE}${c.x}\n`)
  const manifest = readManifest()
  for (const layer of levels(steps)) {
    for (let i = 0; i < layer.length; i += JOBS) {
      // 同层并发；保持日志顺序 → 用 spawnSync 串行的简化实现（音频/NDK 步骤本身已多核）
      layer.slice(i, i + JOBS).forEach(s => runStep(s, manifest))
    }
  }
  const fail = results.filter(r => r.status === 'FAIL')
  log(`\n${c.d}──── 汇总 ────${c.x}`)
  for (const r of results) {
    const col = r.status === 'FAIL' ? c.r : r.status === 'OK' ? c.g : c.d
    log(`  ${r.status.padEnd(6)} ${r.id}${r.why ? '  (' + r.why + ')' : ''}${r.sec ? '  ' + r.sec + 's' : ''}`.replace(/^(\s+)(\w+)/, `$1${col}$2${c.x}`))
  }
  if (fail.length) { log(`\n${c.r}${fail.length} 个步骤失败${c.x}`); process.exit(1) }
  log(`\n${c.g}完成（SKIP 的步骤=本机缺工具链，去 doctor 看缺什么）${c.x}\n`)
}
main().catch(e => { console.error(e); process.exit(1) })
