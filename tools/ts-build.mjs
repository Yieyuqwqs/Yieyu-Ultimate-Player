#!/usr/bin/env node
/**
 * 模块6：TypeScript → out/ts/。
 *
 * 依赖纪律：**只有编译器（typescript）**，没有运行时库、没有打包器（zod / axios / esbuild 全拔了）。
 *   · tsc 把 ts/src 编成 ESM 到 out/ts/esm（编译器是工具链，不是被引用进产物的依赖）
 *   · cleaner.mjs 只是一层再导出，给构建期脚本与回归用
 * 产物：out/ts/esm/**、out/ts/cleaner.mjs
 */
import { spawnSync } from 'node:child_process'
import path from 'node:path'
import fs from 'node:fs'
import { fileURLToPath } from 'node:url'

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..')
const OUT = path.resolve(ROOT, process.env.OUT || 'out')
const ts = path.join(ROOT, 'ts')
const esmOut = path.join(OUT, 'ts', 'esm')
fs.mkdirSync(esmOut, { recursive: true })

const run = (bin, args, cwd) => {
  console.log('  $', bin, args.join(' '))
  const r = spawnSync(bin, args, { cwd, stdio: 'inherit' })
  if (r.status !== 0) { console.error(`失败: ${bin} ${args.join(' ')}`); process.exit(r.status || 1) }
}

if (!fs.existsSync(path.join(ts, 'node_modules'))) {
  run('npm', ['install', '--no-audit', '--no-fund'], ts)
}
run(path.join(ts, 'node_modules/.bin/tsc'), ['-p', 'tsconfig.json', '--outDir', esmOut], ts)

// 再导出层（不打包：没有第三方依赖，直接指向 tsc 产物就行）
const shim = path.join(OUT, 'ts', 'cleaner.mjs')
fs.writeFileSync(shim, `// 构建期再导出（模块6 的产物入口）\nexport * from './esm/clean/index.js'\nexport { SONG_FIELDS } from './esm/schema/fields.js'\n`)
console.log(`  → ${path.relative(ROOT, shim)}`)
