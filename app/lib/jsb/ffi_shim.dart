// 模块24 的 FFI 工具层：**不用 package:ffi**（那是第三方包）。
//
// 这里只做三件事：malloc/free（走 libc）、UTF-8 ↔ Pointer<Utf8> 互转、指针读串。
// 名字刻意与 package:ffi 一致（malloc / Utf8 / toNativeUtf8 / toDartString），
// 这样原有调用点一个都不用改。
import 'dart:convert';
import 'dart:ffi';
import 'dart:typed_data';

/// 与 package:ffi 的 Utf8 同义的 opaque 类型（只用于类型标注）
final class Utf8 extends Opaque {}

final DynamicLibrary _libc = DynamicLibrary.process();
final Pointer<Uint8> Function(int) _mallocFn =
    _libc.lookupFunction<Pointer<Uint8> Function(IntPtr), Pointer<Uint8> Function(int)>('malloc');
final void Function(Pointer<Uint8>) _freeFn =
    _libc.lookupFunction<Void Function(Pointer<Uint8>), void Function(Pointer<Uint8>)>('free');

class _Malloc {
  Pointer<T> allocate<T extends NativeType>(int bytes) {
    final p = _mallocFn(bytes <= 0 ? 1 : bytes);
    if (p == nullptr) throw StateError('malloc 失败');
    return p.cast<T>();
  }

  void free(Pointer p) {
    if (p != nullptr) _freeFn(p.cast<Uint8>());
  }
}

final _Malloc malloc = _Malloc();

extension _Utf8String on String {
  /// UTF-8 编码并补 '\0'（C 侧只认真实字节，且我们永远不传裸指针）
  Pointer<Utf8> toNativeUtf8() {
    final bytes = utf8.encode(this);
    final p = malloc.allocate<Uint8>(bytes.length + 1);
    final view = p.asTypedList(bytes.length + 1);
    view.setAll(0, bytes);
    view[bytes.length] = 0;
    return p.cast<Utf8>();
  }
}

extension _Utf8Pointer on Pointer<Utf8> {
  String toDartString() {
    final u8 = cast<Uint8>();
    var len = 0;
    while (u8[len] != 0) {
      len++;
    }
    return utf8.decode(u8.asTypedList(len), allowMalformed: true);
  }
}
