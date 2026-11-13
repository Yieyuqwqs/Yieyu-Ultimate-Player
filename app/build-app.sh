#!/usr/bin/env bash
# 模块 1/2 出 APK（本地或 CI 同一条路）。
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${YIEYU_OUT:?需要 YIEYU_OUT}"
JNI="$HERE/android/app/src/main/jniLibs/arm64-v8a"
cd "$HERE"

# 1.5) 黑匣子要写 /storage/emulated/0/yieyu → 注入存储权限（幂等）
node patch_manifest.mjs || true

# 1) 安卓壳子：第一次跑才生成（已存在就跳过，不动我们改过的东西）
if [[ ! -d android ]]; then
  echo "→ flutter create 生成安卓壳"
  flutter create --platforms=android --org com.yieyu --project-name yieyu_ultimate .
fi

# 2) minSdk 26（AAudio 硬门槛，模块 15）。Groovy / Kotlin DSL 两种壳都兼容。
if [[ -f android/app/build.gradle ]]; then
  sed -i 's/minSdkVersion flutter.minSdkVersion/minSdkVersion 26/' android/app/build.gradle
  sed -i 's/\(minSdkVersion\) [0-9][0-9]*/\1 26/' android/app/build.gradle
fi
if [[ -f android/app/build.gradle.kts ]]; then
  sed -i 's/minSdk = flutter.minSdkVersion/minSdk = 26/' android/app/build.gradle.kts
fi

# 3) 各语言产物摆进 jniLibs（Flutter 会自动打包）
mkdir -p "$JNI"
for so in "$OUT/native/arm64/libyieyu_core.so" \
          "$OUT/rust/aarch64-linux-android/release/libyieyu_gesture.so" \
          "$OUT/rust/aarch64-linux-android/release/libyieyu_store.so" \
          "$OUT/rust/aarch64-linux-android/release/libyieyu_license.so"; do
  if [[ -f "$so" ]]; then cp -v "$so" "$JNI/"; else echo "⚠ 缺 $so（App 会降级运行）"; fi
done

# ★ 关键：ANDROID_STL=c++_shared 意味着 libyieyu_core.so 依赖 libc++_shared.so，
# 而 gradle 不会自动把它打进 APK → 装机后 dlopen 直接失败（踩过，别删这段）。
if [[ -n "${ANDROID_NDK:-}" ]]; then
  CXX_SHARED="$ANDROID_NDK/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so"
  if [[ -f "$CXX_SHARED" ]]; then
    cp -v "$CXX_SHARED" "$JNI/"
  else
    echo "⚠ 找不到 libc++_shared.so：$CXX_SHARED" >&2
  fi
else
  echo "⚠ 没设 ANDROID_NDK，跳过 libc++_shared.so（装机会 dlopen 失败）" >&2
fi

mkdir -p assets/plugins
cp -v "$OUT"/wasm/*.wasm assets/plugins/ 2>/dev/null || echo "（暂无 wasm 插件，跳过）"

# 4) 出包
flutter pub get
flutter build apk --release --target-platform android-arm64

mkdir -p "$OUT/apk"
cp -v build/app/outputs/flutter-apk/app-release.apk "$OUT/apk/YieyuUltimate-arm64.apk"
ls -lh "$OUT/apk/"
