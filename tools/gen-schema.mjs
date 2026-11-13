#!/usr/bin/env node
/**
 * 【模块 6】构建期 codegen：TS 的 Zod schema → Dart 模型 + 一次真实清洗回归。
 *
 * 为什么这样接：手机上跑不了 Node，所以 TS 层不可能在运行时执行。
 * 它真正的位置是**构建期唯一真源**：字段清单只写一次（ts/src/schema/song.ts 的 SONG_FIELDS），
 * 由这里生成 Dart 模型；同时用编译好的 cleaner.mjs 跑一遍脏样本，
 * 洗不动就直接红 —— 这样 TS 层每次构建都被真执行，不是摆设。
 */
import fs from 'node:fs'
import path from 'node:path'
import { pathToFileURL } from 'node:url'
import { fileURLToPath } from 'node:url'

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..')
const OUT = path.resolve(ROOT, process.env.OUT || 'out')
const BUNDLE = path.join(ROOT, 'ts/src/clean/index.mjs')
const DART_OUT = path.join(ROOT, 'app/lib/model/song.dart')

const die = (m) => { console.error('✗ ' + m); process.exit(1) }

const schemaPath = path.join(ROOT, 'ts/src/schema/fields.mjs')   // 纯数据 ESM，零依赖
if (!fs.existsSync(schemaPath)) die(`没有 ${schemaPath}`)
if (!fs.existsSync(BUNDLE)) die(`没有 ${BUNDLE}`)

const schema = await import(pathToFileURL(schemaPath).href)
const fields = schema.SONG_FIELDS
if (!Array.isArray(fields) || fields.length < 4) die('SONG_FIELDS 缺失或太短')

// ── ① 生成 Dart 模型 ──────────────────────────────────────────────
const dartType = (t) => (t === 'int' ? 'int' : 'String')
const lines = []
lines.push('// 本文件由 tools/gen-schema.mjs 从 ts/src/schema/song.ts 生成，**不要手改**。')
lines.push('// 生成时间：构建期（每次 ./build.sh ts 之后）')
lines.push('')
lines.push('/// 【模块 6】歌曲模型：字段清单唯一真源在 TypeScript 的 Zod schema。')
lines.push('class Song {')
lines.push('  const Song({')
for (const [dart, , t] of fields) lines.push(`    required this.${dart},`)
lines.push('  });')
lines.push('')
for (const [dart, , t] of fields) lines.push(`  final ${dartType(t)} ${dart};`)
lines.push('')
lines.push('  /// 从任意 JSON 对象宽容地取字段：缺字段给默认值，类型不对就当没有。')
lines.push('  static Song? tryParse(Object? node) {')
lines.push('    if (node is! Map) return null;')
lines.push('    final m = <String, Object?>{')
lines.push('      for (final e in node.entries) \'${e.key}\': e.value,')
lines.push('    };')
const args = fields.map(([dart, key, t]) => {
  if (t === 'int') return `      ${dart}: _asInt(m['${key}']),`
  return `      ${dart}: _asStr(m['${key}']),`
})
lines.push('    final id = _asStr(m[\'id\']);')
lines.push('    if (id.isEmpty) return null;')
lines.push('    return Song(')
lines.push(...args)
lines.push('    );')
lines.push('  }')
lines.push('')
lines.push('  static String _asStr(Object? v) => v is String ? v : (v == null ? \'\' : \'$v\');')
lines.push('  static int _asInt(Object? v) {')
lines.push('    if (v is int) return v;')
lines.push('    if (v is num) return v.toInt();')
lines.push('    if (v is String) return int.tryParse(v) ?? 0;')
lines.push('    return 0;')
lines.push('  }')
lines.push('')
lines.push('  Map<String, Object?> toJson() => <String, Object?>{')
for (const [dart, key] of fields) lines.push(`        '${key}': ${dart},`)
lines.push('      };')
lines.push('}')
lines.push('')
fs.mkdirSync(path.dirname(DART_OUT), { recursive: true })
fs.writeFileSync(DART_OUT, lines.join('\n'))
console.log(`  → ${path.relative(ROOT, DART_OUT)}（${fields.length} 字段）`)

// ── ② 脏样本回归：真的把 cleaner.mjs 跑一遍 ────────────────────────
const cleaner = await import(pathToFileURL(BUNDLE).href)
if (typeof cleaner.cleanSourceTrace !== 'function' && typeof cleaner.default !== 'object') {
  die('cleaner.mjs 没导出 cleanSourceTrace（接口变了？）')
}
const dirty = JSON.stringify({
  songs: [
    { id: '1', mid: 'm1', name: '晴天', artist: '周杰伦', album: '叶惠美' },
    { id: '', name: '' },                       // 坏样本：必须被丢掉
    { id: 2, name: 3, artist: null },           // 类型错：必须被丢掉或夹住
    'not-an-object',
  ],
})
const trace = { source: 'wy', elapsedMs: 12, body: dirty }
const out = (cleaner.cleanSourceTrace ?? cleaner.default?.cleanSourceTrace)?.(trace)
if (!Array.isArray(out)) die('cleanSourceTrace 返回的不是数组')
if (out.length === 0) die('脏样本一个都没洗出来（清洗逻辑坏了）')
if (out.some((s) => !s || typeof s !== 'object')) die('清洗结果里混进了非对象')
console.log(`  ✓ 清洗回归：脏样本 4 条 → 干净 ${out.length} 条`)
