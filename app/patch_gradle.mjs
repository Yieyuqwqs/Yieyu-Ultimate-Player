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


// ── [22] 前台服务：把 android/ 编出来的 AAR 塞进 Flutter 工程并拉起 ──────────
// 背景：platform-service 这个 module 一直在编，但 Flutter 工程的 gradle 从来没引用过它，
//      所以 APK 里根本没有 PlaybackService —— 整个模块 22 是"编了不用"。
// AAR 自带的 AndroidManifest 会声明 service 与权限，合并进主包即可。
function patchPlatformService() {
  const aarDir = path.join(root, "..", "android", "platform-service", "build", "outputs", "aar")
  if (!fs.existsSync(aarDir)) { console.log("  ⚠ 没有 platform-service AAR（模块22 走空实现）"); return }
  const aars = fs.readdirSync(aarDir).filter(f => f.endsWith(".aar"))
  if (!aars.length) { console.log("  ⚠ AAR 目录是空的（模块22 走空实现）"); return }

  const libsDir = path.join(androidDir, "app", "libs")
  fs.mkdirSync(libsDir, { recursive: true })
  fs.copyFileSync(path.join(aarDir, aars[0]), path.join(libsDir, "platform-service.aar"))
  console.log("  ✓ 前台服务 AAR 已就位：" + aars[0])

  // ① gradle 依赖（AAR 的传递依赖要显式补，files() 不会带传递依赖）
  for (const name of ["app/build.gradle", "app/build.gradle.kts"]) {
    const p = path.join(androidDir, name)
    if (!fs.existsSync(p)) continue
    let g = fs.readFileSync(p, "utf8")
    if (!g.includes("platform-service.aar")) {
      const kts = name.endsWith(".kts")
      const dep = kts
        ? "    implementation(files(\"libs/platform-service.aar\"))\n" +
          "    implementation(\"androidx.core:core-ktx:1.13.1\")\n" +
          "    implementation(\"androidx.media:media:1.7.0\")\n"
        : "    implementation files(\"libs/platform-service.aar\")\n" +
          "    implementation \"androidx.core:core-ktx:1.13.1\"\n" +
          "    implementation \"androidx.media:media:1.7.0\"\n"
      g = g.replace(/dependencies\s*\{/, (m) => m + "\n" + dep)
      fs.writeFileSync(p, g)
      console.log("  ✓ gradle 已依赖前台服务 AAR：" + name)
    }
  }

  // ② 启动时拉起服务（失败绝不把 App 带崩）
  const kt = path.join(androidDir, "app", "src", "main", "kotlin")
  let mainKt = null
  const walk = (d) => {
    if (!fs.existsSync(d)) return
    for (const e of fs.readdirSync(d, { withFileTypes: true })) {
      const f = path.join(d, e.name)
      if (e.isDirectory()) walk(f)
      else if (e.name === "MainActivity.kt") mainKt = f
    }
  }
  walk(kt)
  if (!mainKt) { console.log("  ⚠ 没找到生成的 MainActivity.kt，跳过服务拉起"); return }
  let k = fs.readFileSync(mainKt, "utf8")
  if (!k.includes("PlaybackService")) {
    k = k.replace(/class MainActivity\s*:\s*FlutterActivity\(\)\s*\{/, m => m + `
    override fun onCreate(savedInstanceState: android.os.Bundle?) {
        super.onCreate(savedInstanceState)
        // [22] 前台服务：通知栏播放控制。任何异常都不许让主界面起不来。
        try {
            com.yieyu.ultimate.service.PlaybackService.start(this)
        } catch (t: Throwable) {
            android.util.Log.w("yieyu", "startForegroundService failed: " + t.message)
        }
    }`)
    fs.writeFileSync(mainKt, k)
    console.log("  ✓ MainActivity 已拉起前台服务")
  }
}

// ── [模块1 平台通道] 文件选择 + 应用目录 ────────────────────────────────
// 为什么自己写：file_picker / path_provider 是第三方 Flutter 插件，各自拖一串
// transitive 依赖（android SDK 版本、生命周期插件…），我们已经因为它们出现过构建告警/失败。
// 做法：把 app/platform/main_activity_channel.kt 追加进生成的 MainActivity.kt，
// 只提供一个 MethodChannel("yieyu/native")：appDir / pickFile。
function patchPlatformChannel() {
  let mainKt = null
  const walk = (d) => {
    if (!fs.existsSync(d)) return
    for (const e of fs.readdirSync(d, { withFileTypes: true })) {
      const f = path.join(d, e.name)
      if (e.isDirectory()) walk(f)
      else if (e.name === 'MainActivity.kt') mainKt = f
    }
  }
  walk(path.join(androidDir, 'app', 'src', 'main', 'kotlin'))
  if (!mainKt) { console.log('  ⚠ 没找到 MainActivity.kt，跳过平台通道'); return }
  let k = fs.readFileSync(mainKt, 'utf8')
  if (k.includes('setupYieyuChannel')) { console.log('  · 平台通道已在 MainActivity 里'); return }

  const snippetPath = path.join(root, 'platform', 'main_activity_channel.kt')
  if (!fs.existsSync(snippetPath)) { console.log('  ⚠ 缺 platform/main_activity_channel.kt'); return }
  const snippet = fs.readFileSync(snippetPath, 'utf8')

  // imports（只加缺的）
  const wantImports = [
    'import android.app.Activity',
    'import android.content.Intent',
    'import android.net.Uri',
    'import io.flutter.plugin.common.MethodChannel',
  ]
  for (const imp of wantImports) {
    if (!k.includes(imp)) k = k.replace(/^(package .*\n)/m, '$1\n' + imp + '\n')
  }
  // onCreate 里注册
  k = k.replace(/override fun onCreate\(savedInstanceState: android\.os\.Bundle\?\) \{/,
    'override fun onCreate(savedInstanceState: android.os.Bundle?) {\n        try { setupYieyuChannel() } catch (_: Throwable) { }')
  k += '\n' + snippet
  fs.writeFileSync(mainKt, k)
  console.log('  ✓ 平台通道已注入 MainActivity（appDir / pickFile）')
}

function patchSigning() {
  const dir = path.join(androidDir, 'app')
  const p = path.join(dir, 'build.gradle')
  if (!fs.existsSync(p)) return
  let s = fs.readFileSync(p, 'utf8')
  const before = s

  // ⚠️ Gradle 8 有硬性顺序：plugins {} 之前只允许 buildscript / pluginManagement / plugins。
  //    把 def keystoreProperties = new Properties() 放在文件最上面会让构建直接失败（踩过）：
  //    "only buildscript {}, pluginManagement {} and other plugins {} script blocks are allowed
  //     before plugins {} blocks"
  //    → 必须插在 plugins {} 块**之后**。
  const header = [
    '',
    '// ── 固定签名（由 app/patch_gradle.mjs 注入，位于 plugins{} 之后，不要手改）',
    'def keystoreProperties = new Properties()',
    "def keystorePropertiesFile = rootProject.file('key.properties')",
    'if (keystorePropertiesFile.exists()) {',
    '    keystoreProperties.load(new FileInputStream(keystorePropertiesFile))',
    '}',
    '',
  ].join('\n')

  if (!s.includes('keystoreProperties')) {
    const m = s.match(/plugins\s*\{[\s\S]*?\}\s*\n/)
    if (m) {
      const at = m.index + m[0].length
      s = s.slice(0, at) + header + s.slice(at)
    } else {
      s = header + s   // 没有 plugins{} 的极老模板，退回顶部
    }
  }

  // 注意：不能写 !s.includes('signingConfigs') —— 模板里的 signingConfigs.debug 会误判成已存在
  if (!/signingConfigs\s*\{/.test(s)) {
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
  // 只有 key.properties 真的在，才切到 release 签名；否则保持 debug，
  // 避免 storeFile=null 让构建直接崩（踩过）
  const hasKey = fs.existsSync(path.join(androidDir, 'key.properties'))
  if (hasKey) {
    s = s.replace(/signingConfig\s*=?\s*signingConfigs\.debug/g, 'signingConfig = signingConfigs.release')
  } else {
    console.log('  ⚠ 没有 key.properties，本次仍用 debug 签名（无法覆盖安装）')
  }

  if (s !== before) { fs.writeFileSync(p, s); console.log('  ✓ 固定签名已接入：build.gradle') }
}

patchBuildGradle()
patchSigning()
patchGradleProperties()
patchPlatformService()
patchPlatformChannel()
