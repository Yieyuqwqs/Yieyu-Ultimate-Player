#!/usr/bin/env bash
# 模块5 出 AAR。gomobile 会强制要求 golang.org/x/mobile 出现在 go.mod 的依赖图里，
# 否则直接 "gomobile bind requires golang.org/x/mobile in the current module"（踩过）。
set -euo pipefail
cd "$(dirname "$0")"
OUT="${YIEYU_OUT:?需要 YIEYU_OUT}"
API="${ANDROID_API:-26}"

echo "→ 挂 golang.org/x/mobile（gomobile 的硬要求）"
go get golang.org/x/mobile/cmd/gobind@latest >/dev/null 2>&1 || go get golang.org/x/mobile@latest

mkdir -p "$OUT/go"
gomobile bind -target=android/arm64 -androidapi "$API" -o "$OUT/go/search.aar" ./internal/bind
ls -lh "$OUT/go/search.aar"
