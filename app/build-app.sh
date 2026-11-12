#!/usr/bin/env bash
# 模块 1/2 出 APK（由 tools/orchestrate.mjs 调用）。
# 关键：把各语言的 .so 摆进 jniLibs，Flutter 侧用 dart:ffi 直接 open。
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${YIEYU_OUT:?需要 YIEYU_OUT}"
JNI="$HERE/android/app/src/main/jniLibs/arm64-v8a"

mkdir -p "$JNI"
for so in "$OUT/native/arm64/libyieyu_core.so" \
          "$OUT/rust/aarch64-linux-android/release/libyieyu_gesture.so" \
          "$OUT/rust/aarch64-linux-android/release/libyieyu_store.so"; do
  [[ -f "$so" ]] || { echo "缺产物: $so（先 ./build.sh native rust）" >&2; exit 1; }
  cp -v "$so" "$JNI/"
done

# libyieyu_license.so 不进 APK：只被 core dlopen —— 但 Android 上 dlopen 也得在包内，
# 所以它同样进 jniLibs，只是 App 不许直接 ffi 它（架构约束，不是技术约束）。
if [[ -f "$OUT/rust/aarch64-linux-android/release/libyieyu_license.so" ]]; then
  cp -v "$OUT/rust/aarch64-linux-android/release/libyieyu_license.so" "$JNI/"
fi

# Wasm 插件与应用资源
mkdir -p "$HERE/assets/plugins"
cp -v "$OUT"/wasm/*.wasm "$HERE/assets/plugins/" 2>/dev/null || echo "（暂无 wasm 插件，跳过）"

cd "$HERE"
flutter pub get
flutter build apk --release --target-platform android-arm64
mkdir -p "$OUT/apk"
cp -v build/app/outputs/flutter-apk/app-release.apk "$OUT/apk/YieyuUltimate-0.1.0-arm64.apk"
