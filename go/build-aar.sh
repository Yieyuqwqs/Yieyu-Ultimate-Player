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
# ① C ABI 共享库：给 C++ 沙箱（模块 7）直接 dlopen 用。
#    这是脚本能真正发 HTTP 的关键 —— 走 Go 的 TLS，不依赖 OpenSSL。
go build -buildmode=c-shared -tags netgo -o "$OUT/go/libyieyu_fetch.so" ./fetch
ls -lh "$OUT/go/libyieyu_fetch.so"

# ② AAR：给 Android/Java 侧（模块 22 及未来的极薄 Kotlin）
gomobile bind -target=android/arm64 -androidapi "$API" -o "$OUT/go/search.aar" ./bind
ls -lh "$OUT/go/search.aar"
