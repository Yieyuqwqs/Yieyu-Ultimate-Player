import 'dart:async';
import 'dart:ffi';
import 'dart:typed_data';

import 'package:ffi/ffi.dart';

/// 【模块 24】JSB 的 Dart 侧。
///
/// 关键事实：Dart ↔ C++ 走 **dart:ffi**，没有 JNI、没有平台通道、没有编解码。
/// 这就是表里"连 JNI 的损耗都降到最低"的真实含义 —— 只有模块 22（Kotlin）才用 JNI。
///
/// 两条流：
///   · 下行 `post(bytes)`：命令（FlatBuffers），UI 线程发出，加锁串行
///   · 上行 `pump`：16ms 拉一次，避免跨 FFI 重入
class YieyuSnapshot {
  const YieyuSnapshot({
    required this.playing,
    required this.sampleRate,
    required this.positionMs,
    required this.durationMs,
    required this.peakL,
    required this.peakR,
    required this.eqDb,
  });

  final bool playing;
  final int sampleRate;
  final int positionMs;
  final int durationMs;
  final double peakL;
  final double peakR;
  final List<double> eqDb;

  static YieyuSnapshot zero() => const YieyuSnapshot(
        playing: false,
        sampleRate: 48000,
        positionMs: 0,
        durationMs: 0,
        peakL: 0,
        peakR: 0,
        eqDb: <double>[],
      );
}

// ── C 结构体（与 core/src/jsb/jsb.h 的 Yieyu_Snapshot 严格一致）────────
final class _CSnapshot extends Struct {
  @Int32()
  external int playing;
  @Int32()
  external int sampleRate;
  @Int64()
  external int positionMs;
  @Int64()
  external int durationMs;
  @Float()
  external double peakL;
  @Float()
  external double peakR;
  @Int32()
  external int eqPoints;
  @Array(31)
  external Array<Float> eqDb;
}

typedef _InitNative = Int32 Function(Pointer<Utf8>, Pointer<Utf8>, Uint32);
typedef _InitDart = int Function(Pointer<Utf8>, Pointer<Utf8>, int);
typedef _PostNative = Int32 Function(Pointer<Uint8>, IntPtr);
typedef _PostDart = int Function(Pointer<Uint8>, int);
typedef _PollNative = IntPtr Function(Pointer<Uint8>, IntPtr);
typedef _PollDart = int Function(Pointer<Uint8>, int);
typedef _SnapNative = Int32 Function(Pointer<_CSnapshot>);
typedef _SnapDart = int Function(Pointer<_CSnapshot>);
typedef _ExportNative = Int32 Function(Pointer<Utf8>, IntPtr);
typedef _ExportDart = int Function(Pointer<Utf8>, int);
typedef _ReloadNative = Int32 Function(Pointer<Int32>, Pointer<Int32>);
typedef _ReloadDart = int Function(Pointer<Int32>, Pointer<Int32>);
typedef _ImportNative = Int32 Function(Pointer<Utf8>, Pointer<Uint8>, Int32);
typedef _ImportDart = int Function(Pointer<Utf8>, Pointer<Uint8>, int);
typedef _IsPublicNative = Int32 Function();
typedef _IsPublicDart = int Function();
typedef _ShutdownNative = Void Function();
typedef _ShutdownDart = void Function();

class JsbClient {
  JsbClient._();

  static final JsbClient instance = JsbClient._();

  static const int abiVersion = 1; // 必须等于 core 的 YIEYU_JSB_ABI

  late final DynamicLibrary _lib = DynamicLibrary.open('libyieyu_core.so');
  late final _InitDart _init =
      _lib.lookupFunction<_InitNative, _InitDart>('Yieyu_Init');
  late final _PostDart _post =
      _lib.lookupFunction<_PostNative, _PostDart>('Yieyu_Post');
  late final _PollDart _poll =
      _lib.lookupFunction<_PollNative, _PollDart>('Yieyu_Poll');
  late final _SnapDart _snapGet =
      _lib.lookupFunction<_SnapNative, _SnapDart>('Yieyu_Snapshot_Get');
  late final _ShutdownDart _shutdown =
      _lib.lookupFunction<_ShutdownNative, _ShutdownDart>('Yieyu_Shutdown');

  // ── 黑匣子 / 热更（模块 0 与 7）────────────────────────────────
  late final _ExportDart _export =
      _lib.lookupFunction<_ExportNative, _ExportDart>('Yieyu_ExportLogs');
  late final _ReloadDart _reload =
      _lib.lookupFunction<_ReloadNative, _ReloadDart>('Yieyu_ReloadScripts');
  late final _ImportDart _import =
      _lib.lookupFunction<_ImportNative, _ImportDart>('Yieyu_ImportScript');
  late final _IsPublicDart _logIsPublic =
      _lib.lookupFunction<_IsPublicNative, _IsPublicDart>('Yieyu_LogIsPublic');

  final _snapCtrl = StreamController<YieyuSnapshot>.broadcast();
  final _songCtrl = StreamController<List<Map<String, Object?>>>.broadcast();

  Stream<YieyuSnapshot> get snapshots => _snapCtrl.stream;
  Stream<List<Map<String, Object?>>> get songs => _songCtrl.stream;

  Timer? _timer;
  Pointer<_CSnapshot>? _snapPtr;
  Pointer<Uint8>? _evtPtr;
  static const int _evtCap = 64 * 1024;

  int init(String filesDir, String abi) {
    final dir = filesDir.toNativeUtf8();
    final a = abi.toNativeUtf8();
    try {
      return _init(dir, a, 0);
    } finally {
      malloc.free(dir);
      malloc.free(a);
    }
  }

  /// 下行命令（FlatBuffers 字节）。返回 0 表示已受理。
  int post(Uint8List fbs) {
    final p = malloc.allocate<Uint8>(fbs.length);
    try {
      p.asTypedList(fbs.length).setAll(0, fbs);
      return _post(p, fbs.length);
    } finally {
      malloc.free(p);
    }
  }

  void startPump() {
    _snapPtr ??= malloc.allocate<_CSnapshot>(sizeOf<_CSnapshot>());
    _evtPtr ??= malloc.allocate<Uint8>(_evtCap);
    _timer ??= Timer.periodic(const Duration(milliseconds: 16), (_) => _tick());
  }

  void _tick() {
    // ① 只读快照（进度 / 峰值 / 31 段 EQ）→ 喂给 UI 与 Rive
    final sp = _snapPtr!;
    if (_snapGet(sp) == 0) {
      final n = sp.ref.eqPoints;
      final eq = <double>[
        for (var i = 0; i < n && i < 31; i++) sp.ref.eqDb[i],
      ];
      _snapCtrl.add(YieyuSnapshot(
        playing: sp.ref.playing == 1,
        sampleRate: sp.ref.sampleRate,
        positionMs: sp.ref.positionMs,
        durationMs: sp.ref.durationMs,
        peakL: sp.ref.peakL,
        peakR: sp.ref.peakR,
        eqDb: eq,
      ));
    }

    // ② 事件环：FlatBuffers 事件流（搜索结果 / 错误 / 授权状态）
    final ep = _evtPtr!;
    final got = _poll(ep, _evtCap);
    if (got > 0) {
      // TODO(P1)：flatc 生成的事件解码器接进来（lib/jsb/jsb_ops.dart）
      // 骨架期只保证字节到了 Dart 侧，不解码也不崩。
    }
  }

  /// 导出黑匣子日志。返回最终目录；失败返回 null。
  /// ★ UI 拿到这个路径只允许显示"已导出"，不许把路径/异常原文怼给用户。
  String? exportLogs() {
    final buf = malloc.allocate<Utf8>(600);
    try {
      final rc = _export(buf, 600);
      return rc == 0 ? buf.toDartString() : null;
    } catch (_) {
      return null;
    } finally {
      malloc.free(buf);
    }
  }

  /// 导入一个 .lua 脚本（SAF 选完直接喂字节，用户不用碰存储路径）。
  /// 返回 0=成功；-1 参数错；-2 触发沙箱黑名单；-3 写盘失败；-4 超过 512KB。
  int? importScript(String name, Uint8List bytes) {
    final n = name.toNativeUtf8();
    final buf = malloc.allocate<Uint8>(bytes.length);
    try {
      buf.asTypedList(bytes.length).setAll(0, bytes);
      return _import(n, buf, bytes.length);
    } catch (_) {
      return null;      // 核心没加载成功时，上层会降级提示
    } finally {
      malloc.free(n);
      malloc.free(buf);
    }
  }

  /// 热更音源脚本（模块 7）。返回 (加载数, 被拒数)；失败返回 null。
  (int, int)? reloadScripts() {
    final a = malloc.allocate<Int32>(sizeOf<Int32>());
    final b = malloc.allocate<Int32>(sizeOf<Int32>());
    try {
      a.value = 0;
      b.value = 0;
      final rc = _reload(a, b);
      if (rc != 0) return null;
      return (a.value, b.value);
    } catch (_) {
      return null;
    } finally {
      malloc.free(a);
      malloc.free(b);
    }
  }

  /// 日志是否真的写进了公共目录（/storage/emulated/0/yieyu）
  bool get logIsPublic {
    try {
      return _logIsPublic() == 1;
    } catch (_) {
      return false;
    }
  }

  void dispose() {
    _timer?.cancel();
    _timer = null;
    if (_snapPtr != null) malloc.free(_snapPtr!);
    if (_evtPtr != null) malloc.free(_evtPtr!);
    _snapPtr = null;
    _evtPtr = null;
    _shutdown();
  }
}
