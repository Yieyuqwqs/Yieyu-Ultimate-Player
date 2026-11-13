// [21]+[24] 控制面帧编码（Dart 侧）。**必须**与 core/src/ipc/flatbuf_codec.h 逐字节一致：
//   [0..3] magic "YJSC"   [4..5] 契约版本 u16   [6..7] op u16   [8..11] sampleRate u32
//   [12..15] 载荷长度 u32  [16..23] seq u64  [24..31] ptsUs u64  [32..39] checksum u64
//   [40..]  UTF-8 载荷
// 布局由 tools/verify-idl.mjs 拿 idl/*.fbs 反查；两头对不上 CI 直接红。
import 'dart:convert';
import 'dart:typed_data';

// 顺序 = idl/jsb.fbs 的 enum Op（0=NONE 起算）。改这里必须同时改 fbs 与 core 的 switch，
// tools/verify-idl.mjs 会逐项比对，差一位就红。
enum JsbOp { none, play, pause, next, prev, search, seek, eqPresetNext, setEqBand, license, loadPlugin }

class JsbOps {
  static const int headerBytes = 40;
  static const int abiVersion = 1;

  static Uint8List _frame(JsbOp op, List<int> payload) {
    final out = Uint8List(headerBytes + payload.length);
    final bd = ByteData.view(out.buffer, out.offsetInBytes, out.lengthInBytes);
    out[0] = 0x59; // Y
    out[1] = 0x4A; // J
    out[2] = 0x53; // S
    out[3] = 0x43; // C
    bd.setUint16(4, abiVersion, Endian.little);
    bd.setUint16(6, op.index, Endian.little);
    bd.setUint32(8, 0, Endian.little);
    bd.setUint32(12, payload.length, Endian.little);
    bd.setUint64(16, 0, Endian.little);
    bd.setUint64(24, 0, Endian.little);
    bd.setUint64(32, 0, Endian.little);
    if (payload.isNotEmpty) out.setRange(headerBytes, headerBytes + payload.length, payload);
    return out;
  }

  static Uint8List transport(JsbOp op) => _frame(op, const []);

  static Uint8List search(String keyword) => _frame(JsbOp.search, utf8.encode(keyword));

  static Uint8List play(String songId) => _frame(JsbOp.play, utf8.encode(songId));

  static Uint8List seek(int positionMs) => _frame(JsbOp.seek, utf8.encode('$positionMs'));

  static Uint8List eqPresetNext() => _frame(JsbOp.eqPresetNext, const []);
}
