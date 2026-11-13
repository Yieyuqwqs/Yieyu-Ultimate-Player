#!/usr/bin/env node
/**
 * 【模块 21】契约真源校验：idl/*.fbs 与两端手写实现必须逐字节/逐个对齐。
 *
 * 查三件事（任何一条对不上，CI 直接红 —— 契约不一致比编译错误更可怕，因为它能"编过但跑错"）：
 *   1) frames.fbs 里 FrameHeader 的字段顺序/类型 → 推出偏移，必须等于 flatbuf_codec.h 的 kOff* 常量；
 *   2) frames.fbs 的 file_identifier 必须是 "YFRM"（上行事件帧的 magic）；
 *   3) jsb.fbs 的 enum Op 顺序必须等于 Dart 的 enum JsbOp 顺序（差一位 = 点歌变成暂停）。
 */
import fs from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..')
const fail = (msg) => { console.error('✗ ' + msg); process.exitCode = 1 }
const ok = (msg) => console.log('  ✓ ' + msg)

const read = (p) => fs.readFileSync(path.join(ROOT, p), 'utf8')

// ── 1) frames.fbs → 偏移 ────────────────────────────────────────────
const SIZE = { byte: [1, 1], ubyte: [1, 1], short: [2, 2], ushort: [2, 2], int: [4, 4], uint: [4, 4], long: [8, 8], ulong: [8, 8], float: [4, 4], double: [8, 8] }
const snakeToCamel = (s) => s.replace(/_([a-z])/g, (_, c) => c.toUpperCase())

const frames = read('idl/frames.fbs')
const structStart = frames.indexOf('struct FrameHeader')
if (structStart < 0) fail('frames.fbs 里找不到 struct FrameHeader')
const body = frames.slice(frames.indexOf('{', structStart) + 1, frames.indexOf('}', structStart))
const fields = []
for (const raw of body.split('\n')) {
  const line = raw.replace(/\/\/.*$/, '').trim()
  if (!line) continue
  const m = line.match(/^([a-z_]+)\s*:\s*([a-z]+)\s*;/)
  if (!m) continue
  fields.push({ name: m[1], type: m[2] })
}
let off = 0, maxAlign = 1
const expect = {}
for (const f of fields) {
  const s = SIZE[f.type]
  if (!s) { fail(`frames.fbs 未知类型 ${f.type}`); continue }
  const [size, align] = s
  maxAlign = Math.max(maxAlign, align)
  off = Math.ceil(off / align) * align
  expect['kOff' + snakeToCamel(f.name)[0].toUpperCase() + snakeToCamel(f.name).slice(1)] = off
  off += size
}
const total = Math.ceil(off / maxAlign) * maxAlign

const codec = read('core/src/ipc/flatbuf_codec.h')
const got = {}
for (const m of codec.matchAll(/kOff([A-Za-z]+)\s*=\s*(\d+)/g)) got['kOff' + m[1]] = Number(m[2])
const bytesM = codec.match(/kFrameHeaderBytes\s*=\s*(\d+)/)
const gotBytes = bytesM ? Number(bytesM[1]) : -1

if (gotBytes !== total) fail(`帧头大小不一致：fbs 推出 ${total}，codec 写 ${gotBytes}`)
for (const [k, v] of Object.entries(expect)) {
  if (got[k] !== v) fail(`字段偏移不一致 ${k}：fbs ${v} / codec ${got[k] ?? '缺失'}`)
}
if (expect && Object.keys(got).length) ok(`FrameHeader ${fields.length} 字段 / ${total} 字节，两侧一致`)

// ── 2) file_identifier ────────────────────────────────────────────
const frameId = frames.match(/file_identifier\s+"([^"]+)"/)
if (!frameId || frameId[1] !== 'YFRM') fail(`frames.fbs 的 file_identifier 必须是 "YFRM"（现在：${frameId ? frameId[1] : '缺失'}）`)
else ok('frames.fbs file_identifier = YFRM')

const jsb = read('idl/jsb.fbs')
const cmdId = jsb.match(/file_identifier\s+"([^"]+)"/)
if (!cmdId || cmdId[1] !== 'YJSC') fail(`jsb.fbs 的 file_identifier 必须是 "YJSC"（现在：${cmdId ? cmdId[1] : '缺失'}）`)
else ok('jsb.fbs file_identifier = YJSC')

// ── 3) Op 枚举顺序 ────────────────────────────────────────────────
const opBlock = jsb.slice(jsb.indexOf('enum Op'), jsb.indexOf('}', jsb.indexOf('enum Op')))
const fbsOps = [...opBlock.matchAll(/^\s*([A-Z_]+)\s*=/gm)].map((m) => m[1])
const dart = read('app/lib/jsb/jsb_ops.dart')
const dartOps = (dart.match(/enum JsbOp\s*\{([^}]*)\}/) || [, ''])[1].split(',').map((s) => s.trim()).filter(Boolean)
if (fbsOps.length !== dartOps.length) {
  fail(`Op 数量不一致：fbs ${fbsOps.length} / Dart ${dartOps.length}`)
} else {
  for (let i = 0; i < fbsOps.length; ++i) {
    const norm = (v) => v.toLowerCase().replace(/_/g, '')
    if (norm(fbsOps[i]) !== norm(dartOps[i])) fail(`Op[${i}] 不一致：fbs ${fbsOps[i]} / Dart ${dartOps[i]}`)
  }
  ok(`Op 枚举 ${fbsOps.length} 项，fbs 与 Dart 顺序一致`)
}

// ── 4) 版本号 ─────────────────────────────────────────────────────
const hVersion = (read('core/src/jsb/jsb.h').match(/#define YIEYU_JSB_ABI (\d+)/) || [])[1]
const dVersion = (dart.match(/abiVersion\s*=\s*(\d+)/) || [])[1]
if (!hVersion || hVersion !== dVersion) fail(`ABI 版本不一致：core ${hVersion} / Dart ${dVersion}`)
else ok(`ABI 版本 = ${hVersion}`)

if (process.exitCode) { console.error('✗ 契约校验失败'); process.exit(1) }
console.log('✓ 模块21 契约校验通过')
