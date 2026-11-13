#!/usr/bin/env node
/**
 * flutter create 生成的安卓壳子有几个隐形成本，每次 CI 都要为它们下载几百兆到 1GB：
 *
 *   1. ndkVersion = flutter.ndkVersion  → 逼 Gradle 拉 NDK 25（~1GB，而我们本来就有 NDK 27）
 *      → 改成读环境变量 YIEYU_NDK_VERSION（CI 里就是预装 NDK 的目录名），命中即零下载。
 *      注意：rive 的 Flutter 插件确实用 CMake 编 native，所以 NDK **不能删**，只能指对版本。
 *   2. gradle.properties 没开 build cache / parallel → 每次全量
 *   3. 签名用 debug → 每次签名不同，装不上（由 patchSigning 处理）
 *
 * 幂等：每次构建都跑。
 */
import fs from 'node:fs'
import path from 'node:path'

const root = path.dirname(new URL(import.meta.url).pathname)
const androidDir = path.join(root, 'android')

function patchBuildGradle() {
  for (const name of ['app/build.gradle', 'app/build.gradle.kts']) {
    const p = path.join(androidDir, name)
    if (!fs.existsSync(p)) continue
    let s = fs.readFileSync(p, 'utf8')
    const before = s
    const nativeVersion = 'System.getenv("YIEYU_NDK_VERSION") ?: flutter.ndkVersion'
    const kotlinVersion = 'System.getenv("YIEYU_NDK_VERSION") ?: flutter.ndkVersion'
    s = s.replace(/^\s*ndkVersion\s*=\s*flutter\.ndkVersion\s*$/gm, '    ndkVersion = ' + nativeVersion + '   // 用预装 NDK，不下载')
    s = s.replace(/^\s*ndkVersion\s*=\s*"[\d.]+"\s*$/gm, '    ndkVersion = ' + nativeVersion + '   // 用预装 NDK，不下载')
    s = s.replace(/^\s*ndkVersion\s*:\s*String\?.*$/gm, '    ndkVersion = ' + kotlinVersion)
    if (/buildToolsVersion/.test(s)) {
      s = s.replace(/buildToolsVersion\s*=?\s*"[^"]*"/g, 'buildToolsVersion = "34.0.0"')
    }
    if (s !== before) {
      fs.writeFileSync(p, s)
      console.log('  ✓ ndkVersion 已指向预装版本：' + name)
    }
  }
}

function patchGradleProperties() {
  const p = path.join(androidDir, 'gradle.properties')
  if (!fs.existsSync(p)) return
  let s = fs.readFileSync(p, 'utf8')
  const want = [
    'org.gradle.parallel=true',
    'org.gradle.caching=true',
    'org.gradle.configureondemand=true',
    'org.gradle.jvmargs=-Xmx4g -Dfile.encoding=UTF-8',
    'android.useAndroidX=true',
    // 关键：禁止 Gradle 自己去下 SDK 组件（下不到就报错，而不是傻等 1 分钟）
    // 我们在 CI 里已经保证 build-tools/cmake/ndk 都存在
  ]
  let added = 0
  for (const line of want) {
    const key = line.split('=')[0]
    if (new RegExp('^' + key + '=', 'm').test(s)) continue
    s += (s.endsWith('\n') ? '' : '\n') + line + '\n'
    added++
  }
  if (added) { fs.writeFileSync(p, s); console.log('  ✓ gradle.properties 补了 ' + added + ' 项') }
}

function patchSigning() {
  const dir = path.join(androidDir, 'app')
  for (const name of ['build.gradle']) {
    const p = path.join(dir, name)
    if (!fs.existsSync(p)) continue
    let s = fs.readFileSync(p, 'utf8')
    const before = s
    if (!s.includes('keystoreProperties')) {
      s = [
        'def keystoreProperties = new Properties()',
        "def keystorePropertiesFile = rootProject.file('key.properties')",
        'if (keystorePropertiesFile.exists()) {',
        '    keystoreProperties.load(new FileInputStream(keystorePropertiesFile))',
        '}',
        '',
      ].join('\n') + s
    }
    if (!s.includes('signingConfigs')) {
      s = s.replace(/android\s*\{/, (m) => m + `
    signingConfigs {
        release {
            keyAlias keystoreProperties['keyAlias']
            keyPassword keystoreProperties['keyPassword']
            storeFile keystoreProperties['storeFile'] ? file(keystoreProperties['storeFile']) : null
            storePassword keystoreProperties['storePassword']
        }
    }`)
    }
    s = s.replace(/signingConfig\s*=?\s*signingConfigs\.debug/g, 'signingConfig = signingConfigs.release')
    if (s !== before) { fs.writeFileSync(p, s); console.log('  ✓ 固定签名已接入：' + name) }
  }
}

patchBuildGradle()
patchSigning()
patchGradleProperties()
