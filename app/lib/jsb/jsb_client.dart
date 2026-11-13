import 'dart:async';
import 'dart:convert';
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

typedef _UrlIntNative = Int32 Function(Pointer<Utf8>);
typedef _UrlIntDart = int Function(Pointer<Utf8>);
typedef _I64IntNative = Int32 Function(Int64);
typedef _I64IntDart = int Function(int);
typedef _VoidVoidNative = Void Function();
typedef _VoidVoidDart = void Function();
typedef _FloatVoidNative = Void Function(Float);
typedef _FloatVoidDart = void Function(double);
typedef _ConstCharNative = Pointer<Utf8> Function();
typedef _CallNative = Int32 Function(Pointer<Utf8>, Pointer<Utf8>, Pointer<Utf8>,
    Pointer<Utf8>, Pointer<Utf8>, Pointer<Utf8>, Int32);
typedef _CallDart = int Function(Pointer<Utf8>, Pointer<Utf8>, Pointer<Utf8>,
    Pointer<Utf8>, Pointer<Utf8>, Pointer<Utf8>, int);
typedef _ConstCharDart = Pointer<Utf8> Function();
typedef _DirNative = Int32 Function(Pointer<Utf8>, IntPtr);
typedef _DirDart = int Function(Pointer<Utf8>, int);
typedef _StatsNative = Void Function(Pointer<Utf8>, IntPtr);
typedef _IntVoidNative = Void Function(Int32);
typedef _IntVoidDart = void Function(int);
typedef _IntFloatVoidNative = Void Function(Int32, Float);
typedef _IntFloatVoidDart = void Function(int, double);
typedef _ToneVoidNative = Void Function(Int32, Int32, Int32, Float);
typedef _ToneVoidDart = void Function(int, int, int, double);
typedef _StatsDart = void Function(Pointer<Utf8>, int);
// [5] 搜歌 / 点歌
typedef _SearchNative = Int32 Function(Pointer<Utf8>);
typedef _SearchDart = int Function(Pointer<Utf8>);
typedef _PlaySongNative = Int32 Function(Pointer<Utf8>, Pointer<Utf8>, Pointer<Utf8>);
typedef _PlaySongDart = int Function(Pointer<Utf8>, Pointer<Utf8>, Pointer<Utf8>);
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

  // ── 播放控制（能放歌的最小闭环：URI → 解码 → AAudio）─────────────
  // 注意：playUrl 只是**异步受理**，返回 0 不代表已经出声。
  late final _UrlIntDart _playUrl =
      _lib.lookupFunction<_UrlIntNative, _UrlIntDart>('Yieyu_PlayUrl');
  late final _VoidVoidDart _pause =
      _lib.lookupFunction<_VoidVoidNative, _VoidVoidDart>('Yieyu_Pause');
  late final _VoidVoidDart _resume =
      _lib.lookupFunction<_VoidVoidNative, _VoidVoidDart>('Yieyu_Resume');
  late final _VoidVoidDart _stop =
      _lib.lookupFunction<_VoidVoidNative, _VoidVoidDart>('Yieyu_Stop');
  late final _I64IntDart _seek =
      _lib.lookupFunction<_I64IntNative, _I64IntDart>('Yieyu_SeekMs');
  late final _IntVoidDart _setDsp =
      _lib.lookupFunction<_IntVoidNative, _IntVoidDart>('Yieyu_SetDsp');
  late final _IntFloatVoidDart _setEqGain =
      _lib.lookupFunction<_IntFloatVoidNative, _IntFloatVoidDart>('Yieyu_SetEqGain');
  late final _ToneVoidDart _setTone =
      _lib.lookupFunction<_ToneVoidNative, _ToneVoidDart>('Yieyu_SetTone');
  late final _DirDart _logDir =
      _lib.lookupFunction<_DirNative, _DirDart>('Yieyu_LogDir');
  late final _CallDart _scriptCallFn =
      _lib.lookupFunction<_CallNative, _CallDart>('Yieyu_ScriptCall');
  late final _ConstCharDart _lastError =
      _lib.lookupFunction<_ConstCharNative, _ConstCharDart>('Yieyu_LastError');
  late final _StatsDart _stats =
      _lib.lookupFunction<_StatsNative, _StatsDart>('Yieyu_Stats');
  late final _FloatVoidDart _setVolume =
      _lib.lookupFunction<_FloatVoidNative, _FloatVoidDart>('Yieyu_SetVolume');
  late final _IsPublicDart _logIsPublic =
      _lib.lookupFunction<_IsPublicNative, _IsPublicDart>('Yieyu_LogIsPublic');
  // ── [5] 搜歌 / 点歌（真实网络在 Go 里，结果走事件通道回来）──────────
  late final _SearchDart _searchFn =
      _lib.lookupFunction<_SearchNative, _SearchDart>('Yieyu_Search');
  late final _PlaySongDart _playSongFn =
      _lib.lookupFunction<_PlaySongNative, _PlaySongDart>('Yieyu_PlaySong');

  final _snapCtrl = StreamController<YieyuSnapshot>.broadcast();
  final _songCtrl = StreamController<List<Map<String, Object?>>>.broadcast();
  final _msgCtrl = StreamController<String>.broadcast();   // 搜索进度/失败原因（只给界面说人话）

  Stream<YieyuSnapshot> get snapshots => _snapCtrl.stream;
  Stream<List<Map<String, Object?>>> get songs => _songCtrl.stream;
  Stream<String> get messages => _msgCtrl.stream;

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

    // ② 事件帧：'S'=搜索结果 JSON / 'E'=错误文案
    //    帧格式 [0]=类型 [1..4]=小端长度 [5..]=UTF-8（与 core/src/pipeline/engine.cpp 一致）
    final ep = _evtPtr!;
    final got = _poll(ep, _evtCap);
    if (got < 5) return;
    final view = ep.asTypedList(got);
    final type = view[0];
    final len = view[1] | (view[2] << 8) | (view[3] << 16) | (view[4] << 24);
    if (len <= 0 || 5 + len > got) return;
    final payload = utf8.decode(view.sublist(5, 5 + len), allowMalformed: true);

    if (type == 0x53) {
      // 'S'：搜索结果（JSON 数组）
      try {
        final raw = jsonDecode(payload);
        final list = <Map<String, Object?>>[];
        if (raw is List) {
          for (final e in raw) {
            if (e is Map) list.add(Map<String, Object?>.from(e));
          }
        }
        _songCtrl.add(list);
        _msgCtrl.add(list.isEmpty ? '没搜到，换个关键词试试' : '找到 ${list.length} 首');
      } catch (_) {
        _msgCtrl.add('结果解析失败');
      }
    } else if (type == 0x45) {
      // 'E'：错误文案（只进开发者面板/状态行，不进正常 UI 的错误弹窗）
      _msgCtrl.add('搜索失败');
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

  // ── [5] 搜歌 / 点歌 ─────────────────────────────────────────────
  /// 搜歌：0=已受理，结果从 [songs] 流回来（真网络，在 Go 的线程里跑）。
  int search(String keyword) {
    final p = keyword.toNativeUtf8();
    try {
      return _searchFn(p);
    } catch (_) {
      return -1;
    } finally {
      malloc.free(p);
    }
  }

  /// 点歌：source/id/mid 来自搜索结果。直链解析在解码线程，返回 0 只代表受理。
  int playSong(String source, String musicId, String mid) {
    final a = source.toNativeUtf8();
    final b = musicId.toNativeUtf8();
    final c = mid.toNativeUtf8();
    try {
      return _playSongFn(a, b, c);
    } catch (_) {
      return -1;
    } finally {
      malloc.free(a);
      malloc.free(b);
      malloc.free(c);
    }
  }

  // ── 播放控制 ────────────────────────────────────────────────────
  /// 播放一个音频直链或本地路径。0=已受理；<0=参数错；核心没起来时 -1。
  int playUrl(String uri) {
    final p = uri.toNativeUtf8();
    try {
      return _playUrl(p);
    } catch (_) {
      return -1;
    } finally {
      malloc.free(p);
    }
  }

  void pause() {
    try { _pause(); } catch (_) {}
  }

  void resume() {
    try { _resume(); } catch (_) {}
  }

  void stop() {
    try { _stop(); } catch (_) {}
  }

  /// 跳转到 ms 位置。0=已受理。
  int seekMs(int ms) {
    try { return _seek(ms); } catch (_) { return -1; }
  }

  /// 音量 0..1。
  void setVolume(double v) {
    try { _setVolume(v.clamp(0.0, 1.0)); } catch (_) {}
  }
  // ── [17] DSP 音效链（默认全旁路；打开才有音色变化）────────────────
  void setDsp(bool on) {
    try { _setDsp(on ? 1 : 0); } catch (_) {}
  }

  /// 单段 EQ 增益（band 0..30，±24 dB）。
  void setEqGain(int band, double db) {
    try { _setEqGain(band, db.clamp(-24.0, 24.0).toDouble()); } catch (_) {}
  }

  void setTone(bool tube, bool vinyl, bool lpf, double drive) {
    try {
      _setTone(tube ? 1 : 0, vinyl ? 1 : 0, lpf ? 1 : 0, drive);
    } catch (_) {}
  }

  /// 运行目录（日志/脚本都在里面）。**Dart 侧唯一的路径来源**，不许自己拼 /storage/…
  String logDir() {
    final buf = malloc.allocate<Utf8>(600);
    try {
      final rc = _logDir(buf, 600);
      return rc == 0 ? buf.toDartString() : '';
    } catch (_) {
      return '';
    } finally {
      malloc.free(buf);
    }
  }

  /// 调脚本里的函数（音源脚本的 get_music_url 就走这里）。返回 (rc, 结果串)。
  (int, String) scriptCall(String script, String fn,
      [String? a1, String? a2, String? a3]) {
    final sp = script.toNativeUtf8();
    final fp = fn.toNativeUtf8();
    final p1 = a1?.toNativeUtf8();
    final p2 = a2?.toNativeUtf8();
    final p3 = a3?.toNativeUtf8();
    final buf = malloc.allocate<Utf8>(4096);
    try {
      final rc = _scriptCallFn(sp, fp, p1 ?? nullptr, p2 ?? nullptr, p3 ?? nullptr, buf, 4096);
      return (rc, buf.toDartString());
    } catch (_) {
      return (-1, '');
    } finally {
      malloc.free(sp);
      malloc.free(fp);
      malloc.free(buf);
      if (p1 != null) malloc.free(p1);
      if (p2 != null) malloc.free(p2);
      if (p3 != null) malloc.free(p3);
    }
  }

  /// 最近一次失败的**技术原因**（只在开发者面板/诊断文件里出现，绝不进正常界面）。
  String lastError() {
    try {
      final p = _lastError();
      return p == nullptr ? '' : p.toDartString();
    } catch (_) {
      return '';
    }
  }

  /// 各模块健康度一行（含播放器 st= 与 underrun=）。核心没起来时返回空串。
  String statsLine() {
    final buf = malloc.allocate<Utf8>(512);
    try {
      _stats(buf, 512);
      return buf.toDartString();
    } catch (_) {
      return '';
    } finally {
      malloc.free(buf);
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
