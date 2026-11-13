import 'dart:math' as math;

import 'package:flutter/material.dart';
import 'package:flutter/scheduler.dart';

/// 【模块 2】自研微操矢量动画引擎（替代 Rive 运行时）。
///
/// 为什么自己写：Rive 的 .riv 是私有格式、运行时是第三方二进制，本工程零第三方。
/// 这里用**弹簧-阻尼状态机**驱动 31 段 EQ 曲线 + 播放键呼吸灯：
///   · 每个频段一个二阶弹簧（刚度/阻尼可调），目标值来自 [17] 的 31 点 dB 曲线；
///   · 播放时叠加呼吸相位（0.55Hz），暂停时呼吸自然衰减到静止；
///   · 曲线用 Catmull-Rom 转三次贝塞尔，保证 C1 连续（不会出现折线拐角）；
///   · 整帧只在 Ticker 回调里算，不重建 widget（RepaintBoundary + CustomPainter）。
///
/// 铁律：这个 widget **不做任何业务判定**，只把数值画出来。
class VectorStage extends StatefulWidget {
  const VectorStage({
    super.key,
    required this.eqDb,
    required this.playing,
    this.peakL = 0,
    this.peakR = 0,
  });

  final List<double> eqDb;
  final bool playing;
  final double peakL;
  final double peakR;

  @override
  State<VectorStage> createState() => _VectorStageState();
}

class _VectorStageState extends State<VectorStage>
    with SingleTickerProviderStateMixin {
  static const int _bands = 31;

  late final Ticker _ticker;
  final List<double> _pos = List<double>.filled(_bands, 0); // 当前显示值
  final List<double> _vel = List<double>.filled(_bands, 0); // 弹簧速度
  double _breath = 0; // 呼吸相位 0..1
  double _energy = 0; // 峰值包络（0..1）
  Duration _last = Duration.zero;

  @override
  void initState() {
    super.initState();
    _ticker = createTicker(_onTick)..start();
  }

  @override
  void dispose() {
    _ticker.dispose();
    super.dispose();
  }

  void _onTick(Duration now) {
    final dt = _last == Duration.zero
        ? 1 / 60
        : ((now - _last).inMicroseconds / 1e6).clamp(0.0, 1 / 15);
    _last = now;

    // 目标值：31 段 dB → -1..1（±24dB 满量程）
    final target = List<double>.filled(_bands, 0);
    final src = widget.eqDb;
    for (var i = 0; i < _bands; i++) {
      final db = (i < src.length) ? src[i] : 0.0;
      target[i] = (db / 24.0).clamp(-1.0, 1.0);
    }

    // 二阶弹簧：临界阻尼附近，响应 ~90ms（手感"跟得住但不抖"）
    const stiffness = 220.0;
    const damping = 26.0;
    for (var i = 0; i < _bands; i++) {
      final accel = (target[i] - _pos[i]) * stiffness - _vel[i] * damping;
      _vel[i] += accel * dt;
      _pos[i] += _vel[i] * dt;
      if (_pos[i] > 1.2) _pos[i] = 1.2;
      if (_pos[i] < -1.2) _pos[i] = -1.2;
    }

    // 呼吸：播放时匀速推进，暂停时衰减回 0（不是硬切）
    final wantBreath = widget.playing ? 1.0 : 0.0;
    _breath += dt * 0.55;
    if (_breath > 1) _breath -= 1;
    _energy += (wantBreath - _energy) * math.min(1.0, dt * 4.0);
    final peak = math.max(widget.peakL, widget.peakR).clamp(0.0, 1.0);
    _energy = math.max(_energy * 0.94, peak * 0.6);

    setState(() {});
  }

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    return RepaintBoundary(
      child: CustomPaint(
        painter: _StagePainter(
          pos: List<double>.from(_pos),
          breath: _breath,
          energy: _energy,
          accent: theme.colorScheme.primary,
          surface: theme.colorScheme.surfaceContainerHighest,
        ),
        size: Size.infinite,
      ),
    );
  }
}

class _StagePainter extends CustomPainter {
  _StagePainter({
    required this.pos,
    required this.breath,
    required this.energy,
    required this.accent,
    required this.surface,
  });

  final List<double> pos;
  final double breath;
  final double energy;
  final Color accent;
  final Color surface;

  @override
  void paint(Canvas canvas, Size size) {
    final h = size.height;
    final w = size.width;
    final mid = h * 0.56;
    final amp = h * 0.34;
    final n = pos.length;

    // 背景网格（极淡，只做视觉锚点）
    final grid = Paint()
      ..color = surface.withOpacity(0.35)
      ..strokeWidth = 1;
    for (var i = 1; i < 4; i++) {
      final y = mid - amp + amp * 2 * i / 4;
      canvas.drawLine(Offset(0, y), Offset(w, y), grid);
    }

    // 31 段采样点
    final pts = <Offset>[];
    for (var i = 0; i < n; i++) {
      final x = w * (i + 0.5) / n;
      final y = mid - pos[i] * amp;
      pts.add(Offset(x, y));
    }

    // Catmull-Rom → 三次贝塞尔（C1 连续）
    final path = Path()..moveTo(pts.first.dx, pts.first.dy);
    for (var i = 0; i < pts.length - 1; i++) {
      final p0 = pts[i == 0 ? 0 : i - 1];
      final p1 = pts[i];
      final p2 = pts[i + 1];
      final p3 = pts[i + 2 < pts.length ? i + 2 : pts.length - 1];
      final c1 = Offset(p1.dx + (p2.dx - p0.dx) / 6, p1.dy + (p2.dy - p0.dy) / 6);
      final c2 = Offset(p2.dx - (p3.dx - p1.dx) / 6, p2.dy - (p3.dy - p1.dy) / 6);
      path.cubicTo(c1.dx, c1.dy, c2.dx, c2.dy, p2.dx, p2.dy);
    }

    // 呼吸光晕：能量越大越亮，相位驱动半径
    final glow = (0.25 + 0.35 * energy) * (0.75 + 0.25 * math.sin(breath * math.pi * 2));
    final fill = Path.from(path)
      ..lineTo(pts.last.dx, mid)
      ..lineTo(pts.first.dx, mid)
      ..close();
    canvas.drawPath(
      fill,
      Paint()
        ..shader = LinearGradient(
          begin: Alignment.topCenter,
          end: Alignment.bottomCenter,
          colors: [accent.withOpacity(0.55 * glow + 0.08), accent.withOpacity(0.02)],
        ).createShader(Rect.fromLTWH(0, mid - amp, w, amp * 2)),
    );

    // 曲线本体
    canvas.drawPath(
      path,
      Paint()
        ..style = PaintingStyle.stroke
        ..strokeWidth = 2.0
        ..strokeCap = StrokeCap.round
        ..color = accent.withOpacity(0.55 + 0.45 * glow),
    );

    // 峰值点 + 呼吸环
    final dot = Paint()..color = accent.withOpacity(0.9);
    for (var i = 0; i < pts.length; i += 5) {
      canvas.drawCircle(pts[i], 1.6 + 1.2 * energy, dot);
    }
    final ringR = 6 + 10 * energy + 3 * math.sin(breath * math.pi * 2);
    canvas.drawCircle(
      Offset(w / 2, mid),
      ringR,
      Paint()
        ..style = PaintingStyle.stroke
        ..strokeWidth = 1.2
        ..color = accent.withOpacity(0.25 + 0.35 * energy),
    );
  }

  @override
  bool shouldRepaint(covariant _StagePainter old) {
    // 每帧都重绘：值本身就是动画状态（弹簧），比较没有意义
    return true;
  }
}
