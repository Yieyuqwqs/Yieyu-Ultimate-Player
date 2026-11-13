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

option(YIEYU_OFFLINE_ASSETS "没有 third_party 时编空实现，让骨架先过" ON)
option(YIEYU_OBFUSCATE "模块11 Obfuscator-LLVM 混淆" OFF)

set(YIEYU_OUT "$ENV{YIEYU_OUT}" CACHE PATH "各语言产物落位（out/）")
if(NOT YIEYU_OUT)
  set(YIEYU_OUT "${CMAKE_CURRENT_SOURCE_DIR}/../out")
endif()
set(YIEYU_OUT ${YIEYU_OUT} CACHE PATH "" FORCE)
message(STATUS "YIEYU_OUT = ${YIEYU_OUT}   offline=${YIEYU_OFFLINE_ASSETS}  obfuscate=${YIEYU_OBFUSCATE}")

include(cmake/ObfuscatorLLVM.cmake)

# ─── 跨语言产物：全部按 IMPORTED 目标接入，路径约定唯一 ───────────────
set(_rust "${YIEYU_OUT}/rust/aarch64-linux-android/release")
foreach(_lib yieyu_license yieyu_store)
  if(EXISTS "${_rust}/lib${_lib}.so")
    add_library(${_lib} SHARED IMPORTED)
    set_target_properties(${_lib} PROPERTIES IMPORTED_LOCATION "${_rust}/lib${_lib}.so")
    if(_lib STREQUAL "yieyu_license")
      # 让 C++ 侧知道"可以真调用验签库"，不再硬编码 licensed=true
      add_compile_definitions(YIEYU_HAVE_LICENSE=1)
    elseif(_lib STREQUAL "yieyu_store")
      add_compile_definitions(YIEYU_HAVE_STORE=1)
    endif()
  else()
    message(WARNING "缺 ${_lib}.so（先 ./build.sh rust）→ 该模块走空实现")
  endif()
endforeach()

set(_zig "${YIEYU_OUT}/zig/lib/libyieyu_mem.a")
if(NOT EXISTS "${_zig}")
  set(_zig "${YIEYU_OUT}/zig/libyieyu_mem.a")
endif()
if(EXISTS "${_zig}")
  # 让 JSB 知道"池子可用"，命令帧才会走 Zig 的 slab
  add_compile_definitions(YIEYU_HAVE_MEM=1)
endif()

# ─── [7] LuaJIT：脚本引擎。有就真跑，没有就 stub（导入链路照常）──────
set(LUAJIT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/lua")
if(EXISTS "${LUAJIT_ROOT}/lib/libluajit.a" AND EXISTS "${LUAJIT_ROOT}/include/lua.h")
  add_library(luajit STATIC IMPORTED)
  set_target_properties(luajit PROPERTIES IMPORTED_LOCATION "${LUAJIT_ROOT}/lib/libluajit.a")
  include_directories("${LUAJIT_ROOT}/include")
  add_compile_definitions(YIEYU_HAVE_LUAJIT=1)
  message(STATUS "[7] LuaJIT 已接入：脚本可以真正执行")
else()
  message(WARNING "[7] 没有 third_party/lua（LuaJIT）→ 脚本只能导入+校验，执行返回 -2")
endif()

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

# ─── 第三方（见 third_party/README.md）────────────────────────────────
if(NOT YIEYU_OFFLINE_ASSETS)
  # [14] SpeexDSP（BSD-3，替代 JUCE）：头文件 + 预编译静态库
  set(SPEEXDSP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/speexdsp/${ANDROID_ABI}"
      CACHE PATH "SpeexDSP 预编译根目录")
  if(EXISTS "${SPEEXDSP_ROOT}/include/speex/speex_resampler.h")
    include_directories("${SPEEXDSP_ROOT}/include")
    find_library(SPEEXDSP_LIB NAMES speexdsp HINTS "${SPEEXDSP_ROOT}/lib" REQUIRED)
  else()
    message(WARNING "缺 SpeexDSP 预编译 → 模块 14 走旁路（44.1k 直通）")
  endif()
endif()

# ─── 源文件：按模块分组，一眼看得见谁是谁 ─────────────────────────────
set(SRC_COMMON  src/common/blackbox.cpp
                src/common/app_paths.cpp            # 路径唯一真相
                src/common/sha256.cpp               # [11] 自校验摘要
                src/common/integrity.cpp)           # [11] + [12] 常数时间比对
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
set(SRC_STORE   src/store/kv_store.cpp)                # [20] 自研追加日志（唯一写者）
set(SRC_IPC     src/ipc/flatbuf_codec.cpp)             # [21]
set(SRC_SCRIPT  src/script/lua_sandbox.cpp)            # [7]
set(SRC_WASM    src/wasm/wasm_host.cpp)                # [8] 自研 wasm MVP 解释器
set(SRC_ASM     ../asm/neon_ct_compare.S)              # [12]
set(SRC_AI      src/ai/vocals.cpp)                     # [18] 离线人声消除（Python 生成核）

add_library(yieyu_core SHARED
  ${SRC_COMMON} ${SRC_JSB} ${SRC_PIPE} ${SRC_AUDIO} ${SRC_DSP}
  ${SRC_SENSOR} ${SRC_NET} ${SRC_STORE} ${SRC_IPC} ${SRC_SCRIPT} ${SRC_WASM} ${SRC_AI})

# [12] NEON 汇编只在 arm64 参与
if(ANDROID_ABI STREQUAL "arm64-v8a" OR CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64")
  target_sources(yieyu_core PRIVATE ${SRC_ASM})
  target_compile_definitions(yieyu_core PRIVATE YIEYU_NEON=1)
endif()

# ─── 头文件路径 ───────────────────────────────────────────────────────
target_include_directories(yieyu_core PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/lua/src
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
if(NOT YIEYU_OFFLINE_ASSETS)
  list(APPEND YIEYU_LIBS avformat avcodec avutil swresample curl sqlite3 wasmedge)
  if(SPEEXDSP_LIB)
    list(APPEND YIEYU_LIBS ${SPEEXDSP_LIB})
  endif()
endif()
# ★ Rust 的 license/store **不做静态链**：cdylib 没有 SONAME，链进来会把绝对路径写进
#   DT_NEEDED（CI 15.5 就是拦这个，装机 dlopen 必炸）。改为运行时 dlopen，
#   由 YIEYU_HAVE_LICENSE / YIEYU_HAVE_STORE 控制调用点是否编译（见 engine.cpp）。
#   文件仍会被 app/build-app.sh 拷进 jniLibs，所以装机后一定找得到。
if(EXISTS "${_zig}")
  list(APPEND YIEYU_LIBS "${_zig}")
endif()

if(TARGET luajit)
  list(APPEND YIEYU_LIBS luajit m)
endif()
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
if(YIEYU_OFFLINE_ASSETS)
  target_compile_definitions(yieyu_core PRIVATE YIEYU_OFFLINE=1)
  message(WARNING "YIEYU_OFFLINE=ON：FFmpeg/JUCE/curl/wasmedge 均为空实现，仅验证骨架可编")
endif()

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
