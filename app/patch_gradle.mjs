#!/usr/bin/env node
/**
 * flutter create 生成的安卓壳子有三个"隐形成本"，每次 CI 都要为它们下载几百兆到 1GB：
 *
 *   1. ndkVersion = flutter.ndkVersion  → 逼 Gradle 拉 NDK 25（~1GB）
 *      但我们**不用 Gradle 编 native**：C++ 是 CI 里用 CMake + NDK r27 单独交叉编译，
 *      产物直接塞进 jniLibs。所以这行必须删。
 *   2. build-tools / cmake 版本由插件钉死 → 与 runner 预装版本不一致就重新装
 *      改成 runner 预装的版本，直接命中。
 *   3. Gradle 没有开 build cache / parallel → 每次全量
 *      往 gradle.properties 里补上（幂等）。
 *
 * 这个脚本在每次构建时跑，必须幂等。
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
    // 1) 干掉 ndkVersion
    s = s.replace(/^\s*ndkVersion\s*=.*$/gm, '    // ndkVersion 已移除：native 由 CI 的 CMake 交叉编译，不走 Gradle')
    // 2) build-tools 对齐 runner 预装（AGP 8.5 默认 34.0.0）
    if (/buildToolsVersion/.test(s)) {
      s = s.replace(/buildToolsVersion\s*=?\s*"[^"]*"/g, 'buildToolsVersion = "34.0.0"')
    }
    if (s !== before) {
      fs.writeFileSync(p, s)
      console.log('  ✓ 已移除 ndkVersion 钉死：' + name)
    }
  }
}

function patchGradleProperties() {
  const p = path.join(androidDir, 'gradle.properties')
  if (!fs.existsSync(p)) return
  let s = fs.readFileSync(p, 'utf8')
  const want = [
    'org.gradle.parallel=true',
    'org.gradle.caching=true',          // 打开 Gradle build cache（配合 CI 的 ~/.gradle 缓存）
    'org.gradle.configureondemand=true',
    'org.gradle.jvmargs=-Xmx4g -Dfile.encoding=UTF-8',
    'android.useAndroidX=true',
  ]
  let added = 0
  for (const line of want) {
    const key = line.split('=')[0]
    if (new RegExp('^' + key + '=', 'm').test(s)) continue
    s += (s.endsWith('\n') ? '' : '\n') + line + '\n'
    added++
  }
  if (added) {
    fs.writeFileSync(p, s)
    console.log('  ✓ gradle.properties 补了 ' + added + ' 项')
  }
}

function patchSigning() {
  const dir = path.join(androidDir, 'app')
  for (const name of ['build.gradle', 'build.gradle.kts']) {
    const p = path.join(dir, name)
    if (!fs.existsSync(p)) continue
    let s = fs.readFileSync(p, 'utf8')
    const before = s

    if (name.endsWith('.kts')) {
      // Kotlin DSL 壳子（Flutter 3.29+ 才会用），保守处理：不动
      continue
    }

    // ① 顶部读 key.properties
    if (!s.includes('keystoreProperties')) {
      const header = [
        'def keystoreProperties = new Properties()',
        "def keystorePropertiesFile = rootProject.file('key.properties')",
        'if (keystorePropertiesFile.exists()) {',
        '    keystoreProperties.load(new FileInputStream(keystorePropertiesFile))',
        '}',
        '',
      ].join('\n')
      s = header + s
    }

    // ② android {} 里加 signingConfigs
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

    // ③ release 用我们的签名
    s = s.replace(/signingConfig\s*=?\s*signingConfigs\.debug/g, 'signingConfig = signingConfigs.release')

    if (s !== before) {
      fs.writeFileSync(p, s)
      console.log('  ✓ 已接入固定签名：' + name)
    }
  }
}

patchBuildGradle()
patchSigning()
patchGradleProperties()
