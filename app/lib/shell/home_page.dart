import 'package:flutter/material.dart';

import '../animation/rive_stage.dart';
import '../gestures/gesture_bridge.dart';
import '../jsb/jsb_client.dart';
import '../jsb/jsb_ops.dart';

/// 【模块 1】首页：列表框架 + 播放条。
///
/// 这里能看到"三层分工"：
///   · 列表/文字 → 模块 1（Impeller 排版）
///   · 曲线/呼吸灯 → 模块 2（Rive，只吃数值输入）
///   · 手势判定   → 模块 3（Rust/Tokio），Dart 只把原始指针事件转过去
class HomePage extends StatefulWidget {
  const HomePage({super.key});

  @override
  State<HomePage> createState() => _HomePageState();
}

class _HomePageState extends State<HomePage> {
  final List<Map<String, Object?>> _songs = [];
  YieyuSnapshot _snap = YieyuSnapshot.zero();
  String _query = '';

  @override
  void initState() {
    super.initState();
    JsbClient.instance.snapshots.listen((s) => setState(() => _snap = s));
    JsbClient.instance.songs.listen((list) => setState(() {
          _songs
            ..clear()
            ..addAll(list);
        }));
    GestureBridge.instance.load();   // 加载失败不影响界面
  }

  @override
  void dispose() {
    GestureBridge.instance.stop();
    super.dispose();
  }

  void _onTapDown(PointerDownEvent e) => GestureBridge.instance.down(e.pointer, e.position.dx, e.position.dy);
  void _onTapMove(PointerMoveEvent e) => GestureBridge.instance.move(e.pointer, e.position.dx, e.position.dy);
  void _onTapUp(PointerUpEvent e) {
    GestureBridge.instance.up(e.pointer, e.position.dx, e.position.dy);
    final g = GestureBridge.instance.poll();
    switch (g) {
      case 5: // SwipeRight：上一首
        JsbClient.instance.post(JsbOps.transport(JsbOp.prev));
      case 4: // SwipeLeft：下一首
        JsbClient.instance.post(JsbOps.transport(JsbOp.next));
      case 3: // LongPress：切 EQ 预设
        JsbClient.instance.post(JsbOps.eqPresetNext());
      default:
        break;
    }
  }

  @override
  Widget build(BuildContext context) {
    return Listener(
      behavior: HitTestBehavior.translucent,
      onPointerDown: _onTapDown,
      onPointerMove: _onTapMove,
      onPointerUp: _onTapUp,
      child: Scaffold(
        appBar: AppBar(
          title: const Text('Yieyu Ultimate'),
          bottom: PreferredSize(
            preferredSize: const Size.fromHeight(52),
            child: Padding(
              padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 8),
              child: TextField(
                decoration: const InputDecoration(
                  hintText: '搜歌（Go 协程并发）',
                  isDense: true,
                  border: OutlineInputBorder(),
                ),
                onSubmitted: (kw) {
                  _query = kw;
                  JsbClient.instance.post(JsbOps.search(kw));
                },
              ),
            ),
          ),
        ),
        body: Column(
          children: [
            // 【模块 2】曲线是 Rive 画的，输入来自模块 17 的 31 个 dB 点
            SizedBox(
              height: 120,
              child: RiveStage(eqDb: _snap.eqDb, playing: _snap.playing),
            ),
            Expanded(
              child: _songs.isEmpty
                  ? Center(child: Text(_query.isEmpty ? '搜点什么吧' : '搜索中…（$_query）'))
                  : ListView.builder(
                      itemCount: _songs.length,
                      itemBuilder: (_, i) {
                        final s = _songs[i];
                        return ListTile(
                          title: Text('${s['title'] ?? ''}'),
                          subtitle: Text('${s['artist'] ?? ''} · ${s['source'] ?? ''}'),
                          onTap: () => JsbClient.instance.post(JsbOps.play(s['id'] as String)),
                        );
                      },
                    ),
            ),
            _PlayBar(snap: _snap),
          ],
        ),
      ),
    );
  }
}

class _PlayBar extends StatelessWidget {
  const _PlayBar({required this.snap});
  final YieyuSnapshot snap;

  @override
  Widget build(BuildContext context) {
    final secs = (snap.positionMs / 1000).floor();
    final dur = (snap.durationMs / 1000).floor();
    return SafeArea(
      top: false,
      child: Padding(
        padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 8),
        child: Row(
          children: [
            IconButton(
              icon: Icon(snap.playing ? Icons.pause : Icons.play_arrow),
              onPressed: () => JsbClient.instance
                  .post(JsbOps.transport(snap.playing ? JsbOp.pause : JsbOp.play)),
            ),
            Expanded(
              child: LinearProgressIndicator(
                value: snap.durationMs <= 0 ? 0 : snap.positionMs / snap.durationMs,
              ),
            ),
            const SizedBox(width: 12),
            Text('${_t(secs)} / ${_t(dur)}',
                style: Theme.of(context).textTheme.labelSmall),
          ],
        ),
      ),
    );
  }

  static String _t(int s) => '${(s ~/ 60).toString().padLeft(2, '0')}:${(s % 60).toString().padLeft(2, '0')}';
}
