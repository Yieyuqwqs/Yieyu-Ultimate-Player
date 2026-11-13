// 唯一的平台通道：应用目录 + 系统文件选择器。
//
// 为什么要它：原来这两件事靠 path_provider 与 file_picker 两个第三方插件，
// 它们又各自拖一堆 transitive 依赖（android SDK 版本要求、生命周期插件……）。
// 这层只跟 MainActivity 里 50 行 Kotlin 说话，**零第三方包**。
import 'dart:typed_data';

import 'package:flutter/services.dart';

class NativeChannel {
  NativeChannel._();

  static const MethodChannel _ch = MethodChannel('yieyu/native');

  /// App 私有目录（原来 path_provider 的 getApplicationSupportDirectory）
  static Future<String?> appDir() async {
    try {
      return await _ch.invokeMethod<String>('appDir');
    } catch (_) {
      return null;
    }
  }

  /// 系统选择器选一个文件，直接拿字节（原来 file_picker 的 pickFiles）
  static Future<({String name, Uint8List bytes})?> pickFile() async {
    try {
      final res = await _ch.invokeMapMethod<String, Object?>('pickFile');
      if (res == null) return null;
      final name = (res['name'] as String?) ?? 'picked.bin';
      final bytes = res['bytes'];
      if (bytes is! Uint8List || bytes.isEmpty) return null;
      return (name: name, bytes: bytes);
    } catch (_) {
      return null;
    }
  }
}
