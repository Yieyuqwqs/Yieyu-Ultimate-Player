import 'package:flutter/material.dart';
import 'package:rive/rive.dart' as rive;

/// 【模块 2】微操矢量动画：31 段 EQ 曲线 + 播放键呼吸灯。
///
/// 分工铁律：这个 widget **不做任何判定**，只把数值塞进 Rive 状态机的 Number 输入。
/// 曲线怎么动、动多有弹性，是美术在 Rive 编辑器里的事 —— 代码不参与。
class RiveStage extends StatefulWidget {
  const RiveStage({super.key, required this.eqDb, required this.playing});
  final List<double> eqDb;
  final bool playing;

  @override
  State<RiveStage> createState() => _RiveStageState();
}

class _RiveStageState extends State<RiveStage> {
  rive.StateMachineController? _ctrl;
  bool _missingAsset = false;

  @override
  Widget build(BuildContext context) {
    if (_missingAsset) {
      // 降级：美术资源还没进仓时用纯 Flutter 画，别让骨架起不来
      return CustomPaint(painter: _FallbackPainter(widget.eqDb), size: Size.infinite);
    }
    return rive.RiveAnimation.asset(
      'assets/rive/eq_curve.riv',
      stateMachines: const ['EQ'],
      onInit: (artboard) {
        _ctrl = rive.StateMachineController.fromArtboard(artboard, 'EQ');
        artboard.addController(_ctrl!);
      },
      onError: (_) => setState(() => _missingAsset = true),
    );
  }

  @override
  void didUpdateWidget(RiveStage old) {
    super.didUpdateWidget(old);
    final c = _ctrl;
    if (c == null) return;
    for (var i = 0; i < widget.eqDb.length && i < 31; i++) {
      final input = c.findInput<double>('eq$i');
      input?.value = widget.eqDb[i].clamp(-18.0, 18.0);
    }
    c.findInput<double>('breath')?.value = widget.playing ? 1 : 0;
  }
}

class _FallbackPainter extends CustomPainter {
  _FallbackPainter(this.eqDb);
  final List<double> eqDb;

  @override
  void paint(Canvas canvas, Size size) {
    final p = Paint()
      ..color = const Color(0xFF9C89FF)
      ..strokeWidth = 2
      ..style = PaintingStyle.stroke;
    final path = Path();
    for (var i = 0; i < eqDb.length; i++) {
      final x = size.width * i / (eqDb.length - 1 == 0 ? 1 : eqDb.length - 1);
      final y = size.height / 2 - (eqDb[i] / 18.0).clamp(-1.0, 1.0) * size.height / 2.4;
      if (i == 0) path.moveTo(x, y); else path.lineTo(x, y);
    }
    canvas.drawPath(path, p);
  }

  @override
  bool shouldRepaint(_FallbackPainter old) => old.eqDb != eqDb;
}
