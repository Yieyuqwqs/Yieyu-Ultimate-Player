#!/usr/bin/env bash
# 模块 1/2 出 APK。顺序很关键（踩过：补丁步骤跑到 flutter create 之前 → android/ 不存在直接炸）。
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${YIEYU_OUT:?需要 YIEYU_OUT}"
cd "$HERE"

# ── 1. 安卓壳子（第一次才生成）─────────────────────────────────────
if [[ ! -d android ]]; then
  echo "→ flutter create 生成安卓壳"
  flutter create --platforms=android --org com.yieyu --project-name yieyu_ultimate . >/dev/null
fi

# ── 2. 固定签名（必须在 patch_gradle 之前，它要读 key.properties）──
# 密钥以二进制形式入库（app/signing/yieyu.jks），这里只做一次拷贝 —— 不做任何编解码。
if [[ -f "$HERE/signing/yieyu.jks" ]]; then
  mkdir -p android/app
  cp -f "$HERE/signing/yieyu.jks" android/app/yieyu.jks
  cat > android/key.properties <<KEOF
storePassword=yieyu2026
keyPassword=yieyu2026
keyAlias=yieyu
storeFile=yieyu.jks
KEOF
  echo "→ 固定签名已就位（yieyu.jks）"
else
  echo "⚠ 没有 app/signing/yieyu.jks，本次用 debug 签名（无法覆盖安装旧包）"
fi

# ── 3. Gradle 补丁：NDK 指向预装版本 / 签名 / 构建缓存 ──────────────
node patch_gradle.mjs

# ── 4. 存储权限（黑匣子要写 /storage/emulated/0/yieyu）─────────────
node patch_manifest.mjs

# ── 5. minSdk 26（AAudio 硬门槛，模块 15）───────────────────────────
if [[ -f android/app/build.gradle ]]; then
  sed -i 's/minSdkVersion flutter.minSdkVersion/minSdkVersion 26/' android/app/build.gradle
  sed -i 's/\(minSdkVersion\) [0-9][0-9]*/\1 26/' android/app/build.gradle
fi
if [[ -f android/app/build.gradle.kts ]]; then
  sed -i 's/minSdk = flutter.minSdkVersion/minSdk = 26/' android/app/build.gradle.kts
fi

# ── 6. R8 规则：保住 JNI 名字 ───────────────────────────────────────
if [[ -f android/app/proguard-rules.pro ]] && ! grep -q "yieyu.ultimate.service" android/app/proguard-rules.pro 2>/dev/null; then
  cat "$HERE/proguard-yieyu.pro" >> android/app/proguard-rules.pro
  echo "→ 已追加 yieyu R8 规则"
fi

# ── 7. 各语言产物进 jniLibs（Flutter 会自动打包）────────────────────
JNI="android/app/src/main/jniLibs/arm64-v8a"
mkdir -p "$JNI"
for so in "$OUT/native/arm64/libyieyu_core.so" \
          "$OUT/rust/aarch64-linux-android/release/libyieyu_gesture.so" \
          "$OUT/rust/aarch64-linux-android/release/libyieyu_store.so" \
          "$OUT/rust/aarch64-linux-android/release/libyieyu_license.so" \
          "$OUT/go/libyieyu_fetch.so"; do
  if [[ -f "$so" ]]; then cp -v "$so" "$JNI/"; else echo "⚠ 缺 $so（App 会降级运行）"; fi
done

# ★ libc++_shared.so：ANDROID_STL=c++_shared 时 gradle 不会自动打进去，
#   少了它装机 dlopen 就失败（踩过，别删）
if [[ -n "${ANDROID_NDK:-}" ]]; then
  CXX_SHARED="$ANDROID_NDK/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so"
  if [[ -f "$CXX_SHARED" ]]; then cp -v "$CXX_SHARED" "$JNI/"; else echo "⚠ 找不到 libc++_shared.so" >&2; fi
else
  echo "⚠ 没设 ANDROID_NDK，跳过 libc++_shared.so" >&2
fi

# ── 8. 资源 ─────────────────────────────────────────────────────────
mkdir -p assets/plugins
cp -v "$OUT"/wasm/*.wasm assets/plugins/ 2>/dev/null || echo "（暂无 wasm 插件）"

# ── 9. 出包 ─────────────────────────────────────────────────────────
flutter pub get >/dev/null
flutter build apk --release --target-platform android-arm64

mkdir -p "$OUT/apk"
cp -v build/app/outputs/flutter-apk/app-release.apk "$OUT/apk/YieyuUltimate-arm64.apk"
ls -lh "$OUT/apk/"
