import 'dart:ffi';

/// 【模块 3】Rust/Tokio 手势状态机的 Dart 侧。
///
/// 为什么手指事件要绕一圈去 Rust：手势判定是"多指并发 + 多个超时窗口"的状态机，
/// 用 async timer 描述比在 Dart 里手搓 tick 更不容易错；而且判定逻辑天然可单测。
/// 注意：Tokio 跑在独立 OS 线程，**绝不进 UI 线程**。
class GestureBridge {
  GestureBridge._();
  static final GestureBridge instance = GestureBridge._();

  late final DynamicLibrary _lib = DynamicLibrary.open('libyieyu_gesture.so');
  late final int Function() _start =
      _lib.lookupFunction<Int32 Function(), int Function()>('gesture_start');
  late final void Function(int, double, double) _down = _lib.lookupFunction<
      Void Function(Uint32, Float, Float),
      void Function(int, double, double)>('gesture_down');
  late final void Function(int, double, double) _move = _lib.lookupFunction<
      Void Function(Uint32, Float, Float),
      void Function(int, double, double)>('gesture_move');
  late final void Function(int, double, double) _up = _lib.lookupFunction<
      Void Function(Uint32, Float, Float),
      void Function(int, double, double)>('gesture_up');
  late final int Function() _poll =
      _lib.lookupFunction<Int32 Function(), int Function()>('gesture_poll');

  void start() => _start();
  void stop() {}

  void down(int pointer, double x, double y) => _down(pointer, x, y);
  void move(int pointer, double x, double y) => _move(pointer, x, y);
  void up(int pointer, double x, double y) => _up(pointer, x, y);

  /// 返回 Gesture 枚举值：0 None / 1 Tap / 2 DoubleTap / 3 LongPress /
  /// 4 SwipeLeft / 5 SwipeRight / 6 SwipeUp / 7 SwipeDown / 8 Cancel
  int poll() => _poll();
}
