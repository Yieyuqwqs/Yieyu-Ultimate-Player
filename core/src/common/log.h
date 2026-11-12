// 统一日志。音频线程禁用（模块 15 铁律：AAudio 回调里不许 log）。
#pragma once
#ifdef __ANDROID__
#include <android/log.h>
#define YIEYU_TAG "yieyu"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  YIEYU_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  YIEYU_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, YIEYU_TAG, __VA_ARGS__)
#else
#include <cstdio>
#define LOGI(...) do { std::fprintf(stdout, "[I] " __VA_ARGS__); std::fputc('\n', stdout); } while (0)
#define LOGW(...) do { std::fprintf(stderr, "[W] " __VA_ARGS__); std::fputc('\n', stderr); } while (0)
#define LOGE(...) do { std::fprintf(stderr, "[E] " __VA_ARGS__); std::fputc('\n', stderr); } while (0)
#endif
#define YIEYU_RT_UNSAFE /* 标记：此函数可能在实时线程被调用 */
