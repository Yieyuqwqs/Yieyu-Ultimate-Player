import 'dart:io';

import 'package:path_provider/path_provider.dart';

/// 【诊断】Flutter 侧启动日志。
///
/// 为什么需要它：如果 `libyieyu_core.so` 连 dlopen 都失败，黑匣子（C++ 内部）
/// 根本没机会初始化，错误就彻底丢了 —— 用户只能看到"出了点小状况"。
/// 所以启动阶段的异常必须由 Dart 自己记下来，且能被导出。
///
/// UI 静默铁律：这里的内容**永远不直接显示给用户**，只在"隐秘查错入口"里可见。
class BootLog {
  BootLog._();
  static final BootLog instance = BootLog._();

  File? _file;

  Future<File> _ensure() async {
    if (_file != null) return _file!;
    final dir = await getApplicationSupportDirectory();
    final f = File('${dir.path}/yieyu_boot.log');
    _file = f;
    return f;
  }

  Future<void> write(String line) async {
    try {
      final f = await _ensure();
      final ts = DateTime.now().toIso8601String();
      await f.writeAsString('$ts | $line\n', mode: FileMode.append, flush: true);
    } catch (_) {
      // 连日志都写不了就算了，绝不因日志影响启动
    }
  }

  Future<String> read() async {
    try {
      final f = await _ensure();
      if (!await f.exists()) return '(空)';
      final s = await f.readAsString();
      return s.length > 8000 ? s.substring(s.length - 8000) : s;
    } catch (e) {
      return '(读取失败)';
    }
  }

  /// 把启动日志另存到外部目录，返回是否成功（不返回路径给 UI）
  Future<bool> exportTo(String dirPath) async {
    try {
      final f = await _ensure();
      if (!await f.exists()) return false;
      final dst = Directory(dirPath);
      if (!await dst.exists()) await dst.create(recursive: true);
      await f.copy('${dst.path}/yieyu_boot.log');
      return true;
    } catch (_) {
      return false;
    }
  }
}
