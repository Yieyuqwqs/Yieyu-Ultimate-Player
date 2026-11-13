#!/usr/bin/env bash
# 模块 7/8/9/12~18/20/21/23/24 的 CMake 驱动（由 tools/orchestrate.mjs 调用）
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
: "${ANDROID_NDK:?需要 ANDROID_NDK（去 tools/doctor.mjs 看缺什么）}"
OUT="${YIEYU_OUT:?需要 YIEYU_OUT}"
BLD="$OUT/native/build"
INS="$OUT/native/arm64"

LAUNCHER=""
# 不再用 ccache（外部工具 + 缓存目录），直接编

cmake -S "$HERE" -B "$BLD" -G Ninja $LAUNCHER \
  -DCMAKE_TOOLCHAIN_FILE="$HERE/cmake/android-arm64.cmake" \
  -DCMAKE_BUILD_TYPE=Release \
  -DYIEYU_OUT="$OUT" \
  -DYIEYU_OFFLINE_ASSETS="${YIEYU_OFFLINE_ASSETS:-ON}" \
  -DYIEYU_OBFUSCATE="${YIEYU_OBFUSCATE:-OFF}"

cmake --build "$BLD" -j "${YIEYU_JOBS:-4}"
mkdir -p "$INS"
cp -v "$BLD"/libyieyu_core.so "$INS"/
# ★ 这里**不 strip**：模块 11 的链接后处理（tools/obfuscate.mjs）要先重命名符号表再剥离，
#   顺序反了等于先把符号表扔了，重命名直接变成空操作。
echo "→ $INS/libyieyu_core.so  $(stat -c%s "$INS/libyieyu_core.so" 2>/dev/null || echo '?') bytes"
