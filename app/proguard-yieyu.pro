# Flutter release 会用 R8 裁剪/混淆 Java-Kotlin 层（Dart 侧是 tree-shaking，两回事）。
# 我们的 JNI 是"Kotlin external fun ↔ C++ 导出符号"按名字绑定的，
# 一旦 R8 改了类名/方法名，native 就找不到符号 → 通知栏按钮全哑。
#
# 保留 Kotlin 前台服务模块（模块 22）的全部符号
-keep class com.yieyu.ultimate.service.** { *; }
-keepclasseswithmembernames class * {
    native <methods>;
}
# Flutter 引擎与嵌入层（官方模板默认就有，这里再兜一层）
-keep class io.flutter.** { *; }
-keep class io.flutter.plugins.** { *; }
-dontwarn io.flutter.embedding.**
