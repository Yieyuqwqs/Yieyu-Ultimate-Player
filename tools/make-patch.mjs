#!/usr/bin/env node
/**
 * 生成"增量补丁包"：只含 .so + 清单，几兆大小，供快速替换/重打包。
 *
 * 边界（别自欺）：
 *   · Android 10+ 非 root 设备无法用补丁直接覆盖已安装 APK 里的 .so
 *     （classloader-namespace 只允许加载 /data/app/.../lib 下的库）。
 *   · 真实用途：A. CI 内部换 .so 快速重打包；B. root/adb 可写时覆盖即生效；
 *     C. 出问题时单独取 .so 做符号对比或回滚。
 *   · 真正"App 内导入、不打包、秒生效"的是 Lua 脚本层（模块 7）与 Wasm 插件层（模块 8）。
 */
import fs from 'node:fs'
import path from 'node:path'
import crypto from 'node:crypto'
import { fileURLToPath } from 'node:url'

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..')
const OUT = path.resolve(ROOT, process.env.OUT || 'out')
const PKG = path.join(OUT, 'patch')
const LIB = path.join(PKG, 'jniLibs/arm64-v8a')

fs.mkdirSync(LIB, { recursive: true })

const picked = []
for (const src of [path.join(OUT, 'native/arm64'), path.join(OUT, 'go')]) {   // Rust 已下线：只剩 core 与 Go 取流库
  if (!fs.existsSync(src)) continue
  const stack = [src]
  while (stack.length) {
    const p = stack.pop()
    const st = fs.statSync(p)
    if (st.isDirectory()) { for (const f of fs.readdirSync(p)) stack.push(path.join(p, f)); continue }
    if (!p.endsWith('.so')) continue
    const base = path.basename(p)
    if (picked.some((x) => x.name === base)) continue
    picked.push({ name: base, from: p })
  }
}

const ndk = process.env.ANDROID_NDK || process.env.ANDROID_NDK_HOME || ''
if (ndk) {
  const cxx = path.join(ndk, 'toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so')
  if (fs.existsSync(cxx) && !picked.some((x) => x.name === 'libc++_shared.so')) {
    picked.push({ name: 'libc++_shared.so', from: cxx })
  }
}

const manifest = { git: process.env.GITHUB_SHA || 'local', builtAt: new Date().toISOString(), abi: 'arm64-v8a', files: [] }
for (const f of picked) {
  const buf = fs.readFileSync(f.from)
  fs.writeFileSync(path.join(LIB, f.name), buf)
  manifest.files.push({ name: f.name, bytes: buf.length, sha256: crypto.createHash('sha256').update(buf).digest('hex') })
}
fs.writeFileSync(path.join(PKG, 'manifest.json'), JSON.stringify(manifest, null, 2))

const total = manifest.files.reduce((a, b) => a + b.bytes, 0)
fs.writeFileSync(path.join(PKG, 'README.txt'), [
  'Yieyu Ultimate Player · 增量补丁包',
  'commit: ' + manifest.git,
  'files : ' + manifest.files.length + ' 个，共 ' + (total / 1048576).toFixed(1) + ' MB',
  '',
  '用法：',
  '  A) 只要新的 .so（CI 内部重打包 / 单独调试）→ 取 jniLibs/arm64-v8a/*.so',
  '  B) root 设备 → 覆盖到 /data/app/<pkg>/lib/arm64/ 后重启 App（风险自负）',
  '  C) 普通设备 → 无法热替换 .so（Android 平台限制），请安装完整 APK',
  '',
  '真正可以"App 内导入、不打包、秒生效"的是：',
  '  · 音源脚本（Lua，模块 7）：放到 /storage/emulated/0/yieyu/scripts/*.lua',
  '  · 音效插件（Wasm，模块 8）：后续接热更通道',
  '',
  '每个文件的 sha256 见 manifest.json。',
].join('\n'))

console.log('  → out/patch: ' + manifest.files.length + ' 个 .so，共 ' + (total / 1048576).toFixed(1) + ' MB')
