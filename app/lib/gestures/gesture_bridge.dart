import 'dart:ffi';

/// 【模块 3】Rust/Tokio 手势状态机的 Dart 侧。
///
/// 设计要点：**库不在也要能跑**。骨架期 .so 天天换，任何一个 dlopen 失败
/// 都不许把整个界面带崩（这是真踩过的：libc++_shared 没打进 APK 时
/// initState 抛异常 → 整屏灰）。
class GestureBridge {
  GestureBridge._();
  static final GestureBridge instance = GestureBridge._();

  DynamicLibrary? _lib;
  bool _tried = false;
  String? _err;

  int Function()? _start;
  int Function()? _poll;
  void Function(int, double, double)? _down, _move, _up;

  bool get available => _lib != null;
  String? get error => _err;

  bool load() {
    if (_tried) return available;
    _tried = true;
    try {
      final lib = DynamicLibrary.open('libyieyu_gesture.so');
      _lib = lib;
      _start = lib.lookupFunction<Int32 Function(), int Function()>('gesture_start');
      _poll = lib.lookupFunction<Int32 Function(), int Function()>('gesture_poll');
      _down = lib.lookupFunction<Void Function(Uint32, Float, Float),
          void Function(int, double, double)>('gesture_down');
      _move = lib.lookupFunction<Void Function(Uint32, Float, Float),
          void Function(int, double, double)>('gesture_move');
      _up = lib.lookupFunction<Void Function(Uint32, Float, Float),
          void Function(int, double, double)>('gesture_up');
      _start?.call();
      return true;
    } catch (e) {
      _err = '$e';
      _lib = null;
      return false;
    }
  }

  void stop() {}

  void down(int pointer, double x, double y) => _down?.call(pointer, x, y);
  void move(int pointer, double x, double y) => _move?.call(pointer, x, y);
  void up(int pointer, double x, double y) => _up?.call(pointer, x, y);

  /// 0 None / 1 Tap / 2 DoubleTap / 3 LongPress / 4 SwipeLeft / 5 SwipeRight
  /// 6 SwipeUp / 7 SwipeDown / 8 Cancel。库不可用时恒为 0。
  int poll() => _poll?.call() ?? 0;
}
