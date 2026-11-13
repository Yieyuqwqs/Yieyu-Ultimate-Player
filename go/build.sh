#!/usr/bin/env bash
# 模块 5/9 的唯一 C ABI 出口：libyieyu_fetch.so（Go 编，自带 TLS1.3 + HTTP2）。
# 不再产出 AAR：Android 侧一个 Java/Kotlin 调用点都没有（Flutter 全走 dart:ffi → JSB），
# 那份 AAR 编了没人用，已经删掉（gomobile 也就从 CI 里退出去了）。
set -euo pipefail
cd "$(dirname "$0")"
OUT="${YIEYU_OUT:?需要 YIEYU_OUT}"
mkdir -p "$OUT/go"

# ★ SONAME 必须显式给：否则 CMake 链接时会把绝对路径写进 libyieyu_core.so 的 NEEDED 字段，
#   装机后加载器按那个路径找库 → dlopen 失败 → 整个核心起不来（踩过）。
go build -buildmode=c-shared -tags osusergo \
  -ldflags="-s -w -extldflags=-Wl,-soname,libyieyu_fetch.so" \
  -o "$OUT/go/libyieyu_fetch.so" ./fetch
ls -lh "$OUT/go/libyieyu_fetch.so"
