# ════════════════════════════════════════════════════════════════════════
#  Yieyu-Ultimate-Player · C++ 汇聚层（libyieyu_core.so）
#  这里只做三件事：找齐各语言产物、编模块 7/8/9/12~18/20/21/23/24、藏符号。
#  模块清单见 docs/ARCH_24.md。禁止在此文件里写业务逻辑。
# ════════════════════════════════════════════════════════════════════════
cmake_minimum_required(VERSION 3.24)
project(yieyu_core VERSION 0.1.0 LANGUAGES C CXX ASM)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(YIEYU_OBFUSCATE "模块11 Obfuscator-LLVM 混淆" OFF)

set(YIEYU_OUT "$ENV{YIEYU_OUT}" CACHE PATH "各语言产物落位（out/）")
if(NOT YIEYU_OUT)
  set(YIEYU_OUT "${CMAKE_CURRENT_SOURCE_DIR}/../out")
endif()
set(YIEYU_OUT ${YIEYU_OUT} CACHE PATH "" FORCE)
message(STATUS "YIEYU_OUT = ${YIEYU_OUT}   obfuscate=${YIEYU_OBFUSCATE}")

include(cmake/ObfuscatorLLVM.cmake)

# ─── Rust/Zig 已下线：模块 3/4/10/19 全部改成树内 C++ ─────────────────
add_compile_definitions(YIEYU_HAVE_LICENSE=1 YIEYU_HAVE_STORE=1 YIEYU_HAVE_MEM=1)

# ─── [7] Lua 沙箱：解释器是我们自己的（core/src/script/minilua.cpp），不引 LuaJIT ───

# ─── [9] 脚本 HTTP：Go 编的 libyieyu_fetch.so（自带 TLS，替代 curl）──
set(_fetch "${YIEYU_OUT}/go/libyieyu_fetch.so")
if(EXISTS "${_fetch}")
  add_library(yieyu_fetch SHARED IMPORTED)
  set_target_properties(yieyu_fetch PROPERTIES IMPORTED_LOCATION "${_fetch}")
  add_compile_definitions(YIEYU_HAVE_FETCH=1)
  message(STATUS "[9] 脚本 HTTP 已接入：libyieyu_fetch.so（Go + TLS）")
else()
  message(WARNING "[9] 没有 libyieyu_fetch.so → 脚本的 http_get 只会返回空串")
endif()

# 第三方预编译库全部下线（FFmpeg/SpeexDSP/curl/SQLite/WasmEdge/ORT 都改成自研或 NDK 自带）


# ─── 源文件：按模块分组，一眼看得见谁是谁 ─────────────────────────────
set(SRC_COMMON  src/common/blackbox.cpp
                src/common/app_paths.cpp            # 路径唯一真相
                src/common/sha256.cpp               # [11] 自校验摘要
                src/common/integrity.cpp            # [11] + [12]
                src/common/mem_pool.cpp)            # [4] 定长池（取代 Zig）
set(SRC_JSB     src/jsb/jsb.cpp                        # [24]
                src/jsb/jni_service.cpp)               # [22] 唯一 JNI 面
set(SRC_PIPE    src/pipeline/engine.cpp
                src/pipeline/player.cpp)                # 解码线程 + PCM 环调度
set(SRC_AUDIO   src/audio/demux_platform.c             # [13] 平台兜底：AMediaExtractor+AMediaCodec
                src/audio/resampler.cpp                # [14] 自研多相重采样
                src/audio/aaudio_sink.c                # [15]
                src/audio/dsd.cpp)                     # [16] 自研 DSF/DFF → PCM
set(SRC_DSP     src/dsp/eq31.cpp                       # [17]
                src/dsp/tube_vinyl.cpp                 # [17]
                src/dsp/hrtf_8d.cpp)                   # [17] + [23]
set(SRC_SENSOR  src/sensor/gyro_asensor.cpp)           # [23]
set(SRC_NET     src/net/stream_http.c                 # [9] 自研 socket HTTP/1.1
                src/net/song_uri.cpp)                  # [5]/[9] yieyu://song → 真直链
set(SRC_LICENSE src/license/license.cpp)                # [10] 验签（取代 Rust/ring）
set(SRC_GESTURE src/gesture/gesture.cpp)                # [3] 手势（取代 Rust/Tokio）
set(SRC_STORE   src/store/kv_store.cpp                # [20]
                src/store/store_read.cpp)             # [19] 只读半边（取代 Rust/SQLx）
set(SRC_IPC     src/ipc/flatbuf_codec.cpp)             # [21]
set(SRC_SCRIPT  src/script/lua_sandbox.cpp              # [7] 沙箱（自有解释器）
                src/script/minilua.cpp)                 # [7] Lua 子集解释器（零依赖）
set(SRC_WASM    src/plugin/plugin_host.cpp)            # [8] 自研 YPFX 插件宿主（自有字节码）
set(SRC_ASM     ../asm/neon_ct_compare.S)              # [12]
set(SRC_AI      src/ai/vocals.cpp)                     # [18] 离线人声消除（Python 生成核）

add_library(yieyu_core SHARED
  ${SRC_COMMON} ${SRC_JSB} ${SRC_PIPE} ${SRC_AUDIO} ${SRC_DSP}
  ${SRC_SENSOR} ${SRC_NET} ${SRC_STORE} ${SRC_IPC} ${SRC_SCRIPT} ${SRC_WASM} ${SRC_AI}
  ${SRC_LICENSE} ${SRC_GESTURE})

# [12] NEON 汇编只在 arm64 参与
if(ANDROID_ABI STREQUAL "arm64-v8a" OR CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64")
  target_sources(yieyu_core PRIVATE ${SRC_ASM})
  target_compile_definitions(yieyu_core PRIVATE YIEYU_NEON=1)
endif()

# ─── 头文件路径 ───────────────────────────────────────────────────────
target_include_directories(yieyu_core PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/flatbuffers/include
  ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/eigen
  ${YIEYU_OUT}/gen/cpp
  ${YIEYU_OUT}/gen)

# ─── 铁律：音频核零异常零 RTTI；全库符号默认隐藏 ──────────────────────
target_compile_options(yieyu_core PRIVATE
  -fno-exceptions -fno-rtti -fvisibility=hidden -fvisibility-inlines-hidden
  -Wall -Wextra -Wno-unused-parameter -O3)
target_link_options(yieyu_core PRIVATE
  -Wl,--gc-sections -Wl,--exclude-libs,ALL -Wl,-z,max-page-size=16384
  -Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/cmake/exports.map)

set(YIEYU_LIBS android log dl aaudio)   # aaudio 是 NDK 自带，离线也要链
if(ANDROID)
  # [13] 平台解码后端：mediandk 也是 NDK 自带（API 21+），不引入任何第三方 .so
  list(APPEND YIEYU_LIBS mediandk)
endif()
# （不再链 avformat/avcodec/curl/sqlite3/wasmedge/speexdsp：全部自研）

# Rust 库已不存在：license/store 直接编进 core（见 SRC_LICENSE / SRC_STORE）

if(TARGET yieyu_fetch)
  list(APPEND YIEYU_LIBS yieyu_fetch)
endif()

target_link_libraries(yieyu_core PRIVATE ${YIEYU_LIBS})

# ─── 导出面：只有 JSB 的 8 个符号（其余全藏，配合模块 11 混淆）────────
set_target_properties(yieyu_core PROPERTIES
  OUTPUT_NAME yieyu_core
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON)

# ─── 空实现开关：没有第三方时告诉源码"我在裸跑" ───────────────────────

# ─── 测试 ─────────────────────────────────────────────────────────────
# 测试只在宿主上跑（Android 交叉编译出来的 executable 没有意义）
if(NOT ANDROID)
  enable_testing()
  add_executable(yieyu_smoke tests/smoke_test.cpp)
  target_link_libraries(yieyu_smoke PRIVATE yieyu_core)
  add_test(NAME smoke COMMAND yieyu_smoke)
else()
  message(STATUS "Android 目标：跳过 yieyu_smoke（测试在宿主跑）")
endif()
