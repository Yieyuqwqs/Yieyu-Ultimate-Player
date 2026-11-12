// ⚠️ 本文件最终由 `flatc --dart -o app/lib/jsb/gen idl/jsb.fbs` 生成。
// 骨架期给了一份手写的最小版本，保证 Dart 侧能编过、能被 lint 排除。
// 任何字段改动都必须先改 idl/jsb.fbs，再重新生成 —— 不许在本文件里"顺手加字段"。
import 'dart:typed_data';

enum JsbOp { play, pause, next, prev, search, seek, eqPresetNext, setEqBand, license }

class JsbOps {
  /// 控制面帧头（与 idl/jsb.fbs 的 Cmd 表对应，骨架期用最朴素的编码）
  /// [0..3] magic 'YJSC'  [4] op  [5..7] 预留  [8..] 载荷(UTF-8)
  static Uint8List _frame(JsbOp op, List<int> payload) {
    final out = Uint8List(8 + payload.length);
    out[0] = 0x59; // Y
    out[1] = 0x4A; // J
    out[2] = 0x53; // S
    out[3] = 0x43; // C
    out[4] = op.index;
    out.setRange(8, 8 + payload.length, payload);
    return out;
  }

  static Uint8List transport(JsbOp op) => _frame(op, const []);

  static Uint8List search(String keyword) =>
      _frame(JsbOp.search, Uint8List.fromList(keyword.codeUnits));

  static Uint8List play(String songId) =>
      _frame(JsbOp.play, Uint8List.fromList(songId.codeUnits));

  static Uint8List eqPresetNext() => _frame(JsbOp.eqPresetNext, const []);
}
