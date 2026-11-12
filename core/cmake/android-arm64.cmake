# NDK r27c · arm64-v8a 工具链。minSdk 26 = AAudio 的硬门槛（模块 15）。
set(CMAKE_SYSTEM_NAME Android)
set(CMAKE_SYSTEM_VERSION 26)
set(CMAKE_ANDROID_ARCH_ABI arm64-v8a)
set(CMAKE_ANDROID_NDK $ENV{ANDROID_NDK})
set(CMAKE_ANDROID_STL_TYPE c++_shared)     # 与 JUCE/FFmpeg 预编译库统一
set(CMAKE_ANDROID_ARM_NEON ON)             # 模块 12 与 Eigen 都要
set(CMAKE_C_COMPILER   "$ENV{ANDROID_NDK}/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android26-clang")
set(CMAKE_CXX_COMPILER "$ENV{ANDROID_NDK}/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android26-clang++")
set(CMAKE_ASM_COMPILER "$ENV{ANDROID_NDK}/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android26-clang")
set(CMAKE_AR     "$ENV{ANDROID_NDK}/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-ar")
set(CMAKE_RANLIB "$ENV{ANDROID_NDK}/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-ranlib")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
