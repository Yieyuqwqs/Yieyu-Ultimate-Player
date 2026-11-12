#!/usr/bin/env bash
# ── Yieyu-Ultimate-Player 构建唯一入口 ─────────────────────────────
# 所有语言的编译编排都在 tools/orchestrate.mjs 里，本脚本只负责把环境摆正。
set -euo pipefail
cd "$(dirname "$0")"

# 1) 工具链定位（找不到就让 doctor 去喊）
if [[ -z "${ANDROID_NDK:-}" ]]; then
  for cand in "$HOME/Android/Sdk/ndk"/* "$HOME/Library/Android/sdk/ndk"/* /opt/android-ndk*; do
    [[ -d "$cand" ]] && export ANDROID_NDK="$cand" && break
  done
fi
export ANDROID_API="${ANDROID_API:-26}"
export OUT="${OUT:-$PWD/out}"
export YIEYU_JOBS="${YIEYU_JOBS:-4}"

# 2) 缺 node 就直说（它是唯一脚本运行时）
command -v node >/dev/null || { echo "需要 node >= 18（构建期脚本运行时，非 App 运行时）" >&2; exit 1; }

# 3) 交给调度器
exec node tools/orchestrate.mjs "$@"
