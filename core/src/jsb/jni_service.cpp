// 【模块 22 的 JNI 面 —— 全场仅此一处】
// Kotlin PlaybackService 只调两个 static native：nativePing / nativeAction。
// 其余所有跨语言通信都走 dart:ffi（模块 24），因为 FFI 没有 JNI 的装箱与引用表开销。
#include "jsb.h"
#include <jni.h>

extern "C" JNIEXPORT jint JNICALL
Java_com_yieyu_ultimate_service_PlaybackService_nativePing(JNIEnv*, jclass) {
  return Yieyu_Service_Ping();
}

extern "C" JNIEXPORT void JNICALL
Java_com_yieyu_ultimate_service_PlaybackService_nativeAction(JNIEnv*, jclass, jint id) {
  Yieyu_Service_OnNotificationAction((int)id);
}
