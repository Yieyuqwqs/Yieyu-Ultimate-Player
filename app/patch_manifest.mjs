#!/usr/bin/env node
// flutter create 生成的壳子里没有存储权限，而黑匣子要写 /storage/emulated/0/yieyu。
// 这个脚本在每次构建时幂等注入（不许手改壳子，壳子是 CI 现场生成的）。
import fs from 'node:fs'
import path from 'node:path'

const root = path.dirname(new URL(import.meta.url).pathname)
const man = path.join(root, 'android/app/src/main/AndroidManifest.xml')
if (!fs.existsSync(man)) { console.log('  manifest 不存在，跳过'); process.exit(0) }

const perms = [
  // ★ flutter create 只在 debug/profile 的壳子里加 INTERNET；release 的 main
  //   manifest 没有 → 装机后所有网络（音频直链、Lua http_*）全部失败。必补。
  'android.permission.INTERNET',
  'android.permission.ACCESS_NETWORK_STATE',
  'android.permission.WRITE_EXTERNAL_STORAGE',
  'android.permission.READ_EXTERNAL_STORAGE',
  'android.permission.MANAGE_EXTERNAL_STORAGE',
]
let xml = fs.readFileSync(man, 'utf8')
let added = 0
for (const p of perms) {
  if (xml.includes(`android:name="${p}"`)) continue
  const tag = `    <uses-permission android:name="${p}" />\n`
  xml = xml.replace('    <application', tag + '    <application')
  added++
}
if (added) { fs.writeFileSync(man, xml); console.log(`  manifest 注入 ${added} 个权限`) }
else console.log('  manifest 权限已就绪')
