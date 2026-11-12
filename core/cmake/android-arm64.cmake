# NDK r27 · arm64-v8a。
# 注意：不要手搓编译器路径 —— 直接委托 NDK 自带的 android.toolchain.cmake，
# 它会正确处理 sysroot / libc++ / API level / NEON，手搓那套在 r27 上会踩 STL 与 unwinder。
set(ANDROID_ABI        "arm64-v8a"      CACHE STRING "" FORCE)
set(ANDROID_PLATFORM   "android-26"     CACHE STRING "" FORCE)   # 26 = AAudio 硬门槛（模块 15）
set(ANDROID_STL        "c++_shared"     CACHE STRING "" FORCE)   # 与 SpeexDSP/FFmpeg 预编译统一
set(ANDROID_ARM_NEON   ON               CACHE BOOL   "" FORCE)   # 模块 12 汇编 + Eigen 都要
set(ANDROID_PIE        ON               CACHE BOOL   "" FORCE)

if(NOT DEFINED ENV{ANDROID_NDK} AND NOT DEFINED ANDROID_NDK)
  message(FATAL_ERROR "ANDROID_NDK 未设置")
endif()
set(_ndk "$ENV{ANDROID_NDK}")
if(NOT _ndk)
  set(_ndk "${ANDROID_NDK}")
endif()

include("${_ndk}/build/cmake/android.toolchain.cmake")
