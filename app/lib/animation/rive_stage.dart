import 'package:flutter/material.dart';
import 'package:flutter/services.dart' show rootBundle;
import 'package:rive/rive.dart' as rive;

/// 【模块 2】微操矢量动画：31 段 EQ 曲线 + 播放键呼吸灯。
///
/// 分工铁律：这个 widget **不做任何判定**，只把数值塞进 Rive 状态机的 Number 输入。
/// 曲线怎么动、动多有弹性，是美术在 Rive 编辑器里的事 —— 代码不参与。
///
/// 资源缺失时降级为 CustomPaint：骨架期 `.riv` 还没做，App 不能因此白屏。
class RiveStage extends StatefulWidget {
  const RiveStage({super.key, required this.eqDb, required this.playing});
  final List<double> eqDb;
  final bool playing;

  @override
  State<RiveStage> createState() => _RiveStageState();
}

class _RiveStageState extends State<RiveStage> {
  static const String _asset = 'assets/rive/eq_curve.riv';
  bool? _exists;
  rive.StateMachineController? _ctrl;

  @override
  void initState() {
    super.initState();
    rootBundle.load(_asset).then((_) {
      if (mounted) setState(() => _exists = true);
    }).catchError((_) {
      if (mounted) setState(() => _exists = false);
    });
  }

  @override
  Widget build(BuildContext context) {
    if (_exists != true) {
      return CustomPaint(
        painter: _FallbackPainter(widget.eqDb),
        size: Size.infinite,
        // UI 静默铁律：资源缺失也不说话，只画条淡线。
        child: const SizedBox.shrink(),
      );
    }
    return rive.RiveAnimation.asset(
      _asset,
      stateMachines: const ['EQ'],
      onInit: (artboard) {
        final c = rive.StateMachineController.fromArtboard(artboard, 'EQ');
        if (c == null) return;
        artboard.addController(c);
        _ctrl = c;
        _pushInputs();
      },
    );
  }

  void _pushInputs() {
    final c = _ctrl;
    if (c == null) return;
    for (var i = 0; i < widget.eqDb.length && i < 31; i++) {
      c.findInput<double>('eq$i')?.value = widget.eqDb[i].clamp(-18.0, 18.0);
    }
    c.findInput<double>('breath')?.value = widget.playing ? 1 : 0;
  }

  @override
  void didUpdateWidget(RiveStage old) {
    super.didUpdateWidget(old);
    _pushInputs();
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
    final n = eqDb.length;
    if (n == 0) {
      canvas.drawLine(
        Offset(0, size.height / 2),
        Offset(size.width, size.height / 2),
        p..color = const Color(0x33FFFFFF),
      );
      return;
    }
    for (var i = 0; i < n; i++) {
      final x = size.width * i / (n - 1 == 0 ? 1 : n - 1);
      final y = size.height / 2 - (eqDb[i] / 18.0).clamp(-1.0, 1.0) * size.height / 2.4;
      if (i == 0) {
        path.moveTo(x, y);
      } else {
        path.lineTo(x, y);
      }
    }
    canvas.drawPath(path, p);
  }

  @override
  bool shouldRepaint(_FallbackPainter old) => old.eqDb != eqDb;
}
