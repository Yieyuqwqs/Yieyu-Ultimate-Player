import 'dart:async';

import 'package:flutter/material.dart';

import '../animation/rive_stage.dart';
import '../gestures/gesture_bridge.dart';
import '../jsb/jsb_client.dart';
import '../jsb/jsb_ops.dart';
import 'settings_page.dart';

/// 【模块 1】首页：搜索 + 列表 + 播放条。
///
/// 这一版接的是**真链路**：
///   输入关键字 → Yieyu_Search（Go 发 HTTPS）→ 'S' 事件 → 列表 → 点一首
///   → Yieyu_PlaySong → 解码线程换直链 → AMediaExtractor 解码 → 环 → AAudio 出声。
///
/// 以前这里是"搜歌结果上不了屏"：DrainEvents 直接 return 0，事件永远到不了 Dart。
class HomePage extends StatefulWidget {
  const HomePage({super.key});

  @override
  State<HomePage> createState() => _HomePageState();
}

class _HomePageState extends State<HomePage> {
  final List<Map<String, Object?>> _songs = [];
  final List<StreamSubscription<Object?>> _subs = [];
  final TextEditingController _searchCtrl = TextEditingController();

  YieyuSnapshot _snap = YieyuSnapshot.zero();
  String _query = '';
  String _status = '';

  @override
  void initState() {
    super.initState();
    _subs.add(JsbClient.instance.snapshots.listen((s) {
      if (mounted) setState(() => _snap = s);
    }));
    _subs.add(JsbClient.instance.songs.listen((list) {
      if (!mounted) return;
      setState(() {
        _songs
          ..clear()
          ..addAll(list);
      });
    }));
    _subs.add(JsbClient.instance.messages.listen((m) {
      if (mounted) setState(() => _status = m);
    }));
    GestureBridge.instance.load();   // 加载失败不影响界面
  }

  @override
  void dispose() {
    for (final s in _subs) {
      s.cancel();
    }
    _searchCtrl.dispose();
    GestureBridge.instance.stop();
    super.dispose();
  }

  void _onTapDown(PointerDownEvent e) =>
      GestureBridge.instance.down(e.pointer, e.position.dx, e.position.dy);

  void _onTapMove(PointerMoveEvent e) =>
      GestureBridge.instance.move(e.pointer, e.position.dx, e.position.dy);

  void _onTapUp(PointerUpEvent e) {
    GestureBridge.instance.up(e.pointer, e.position.dx, e.position.dy);
    final g = GestureBridge.instance.poll();
    // ★ 以前这里是 switch 直落：一次滑动会连着发三条命令。改成互斥判断。
    if (g == 4) {
      JsbClient.instance.post(JsbOps.transport(JsbOp.next));
    } else if (g == 5) {
      JsbClient.instance.post(JsbOps.transport(JsbOp.prev));
    } else if (g == 3) {
      JsbClient.instance.post(JsbOps.eqPresetNext());
    }
  }

  void _doSearch(String kw) {
    final k = kw.trim();
    if (k.isEmpty) {
      setState(() => _status = '先输个歌名');
      return;
    }
    setState(() {
      _query = k;
      _status = '搜索中…';
      _songs.clear();
    });
    final rc = JsbClient.instance.search(k);
    if (rc != 0 && mounted) {
      setState(() => _status = '搜索没能发出去');
    }
  }

  void _playSong(Map<String, Object?> s) {
    final src = (s['source'] as String?) ?? 'wy';
    final id = (s['id'] as String?) ?? '';
    final mid = (s['mid'] as String?) ?? '';
    final rc = JsbClient.instance.playSong(src, id, mid);
    if (!mounted) return;
    setState(() => _status = rc == 0 ? '正在起播：${s['name'] ?? ''}' : '点歌没能受理');
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
          actions: [
            IconButton(
              icon: const Icon(Icons.more_vert),
              tooltip: '设置',
              onPressed: () => Navigator.of(context).push(
                MaterialPageRoute(builder: (_) => const SettingsPage()),
              ),
            ),
          ],
          bottom: PreferredSize(
            preferredSize: const Size.fromHeight(52),
            child: Padding(
              padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 8),
              child: TextField(
                controller: _searchCtrl,
                textInputAction: TextInputAction.search,
                decoration: const InputDecoration(
                  hintText: '搜歌（Go 协程 + 真实音源接口）',
                  isDense: true,
                  border: OutlineInputBorder(),
                ),
                onSubmitted: _doSearch,
              ),
            ),
          ),
        ),
        body: Column(
          children: [
            // 【模块 2】曲线是 Rive 画的，输入来自模块 17 的 31 个 dB 点
            // （以前 eqDb 一个字节都没填 → 这条线永远是平的；现在 engine 真的把 31 点填进来了）
            SizedBox(
              height: 120,
              child: RiveStage(eqDb: _snap.eqDb, playing: _snap.playing),
            ),
            Expanded(
              child: _songs.isEmpty
                  ? Center(
                      child: Padding(
                        padding: const EdgeInsets.all(24),
                        child: Text(
                          _status.isNotEmpty
                              ? _status
                              : (_query.isEmpty ? '搜点什么吧' : '没有结果'),
                          textAlign: TextAlign.center,
                          style: const TextStyle(fontSize: 13, color: Colors.white54),
                        ),
                      ),
                    )
                  : ListView.builder(
                      itemCount: _songs.length,
                      itemBuilder: (_, i) {
                        final s = _songs[i];
                        final title = (s['name'] as String?) ?? '';
                        final artist = (s['artist'] as String?) ?? '';
                        final album = (s['album'] as String?) ?? '';
                        return ListTile(
                          dense: true,
                          leading: const Icon(Icons.music_note, size: 20),
                          title: Text(title, maxLines: 1, overflow: TextOverflow.ellipsis),
                          subtitle: Text(
                            album.isEmpty ? artist : '$artist · $album',
                            maxLines: 1,
                            overflow: TextOverflow.ellipsis,
                          ),
                          trailing: IconButton(
                            icon: const Icon(Icons.play_arrow),
                            onPressed: () => _playSong(s),
                          ),
                          onTap: () => _playSong(s),
                        );
                      },
                    ),
            ),
            _PlayBar(
              snap: _snap,
              status: _status,
              onToggle: () {
                if (_snap.playing) {
                  JsbClient.instance.pause();
                } else {
                  JsbClient.instance.resume();
                }
              },
            ),
          ],
        ),
      ),
    );
  }
}

class _PlayBar extends StatelessWidget {
  const _PlayBar({required this.snap, required this.status, required this.onToggle});

  final YieyuSnapshot snap;
  final String status;
  final VoidCallback onToggle;

  @override
  Widget build(BuildContext context) {
    final secs = (snap.positionMs / 1000).floor();
    final dur = (snap.durationMs / 1000).floor();
    return SafeArea(
      top: false,
      child: Padding(
        padding: const EdgeInsets.fromLTRB(16, 4, 16, 8),
        child: Column(
          mainAxisSize: MainAxisSize.min,
          children: [
            if (status.isNotEmpty)
              Align(
                alignment: Alignment.centerLeft,
                child: Padding(
                  padding: const EdgeInsets.only(bottom: 4),
                  child: Text(status,
                      style: const TextStyle(fontSize: 11, color: Colors.white38)),
                ),
              ),
            Row(
              children: [
                IconButton(
                  icon: Icon(snap.playing ? Icons.pause : Icons.play_arrow),
                  onPressed: onToggle,
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
          ],
        ),
      ),
    );
  }

  static String _t(int s) =>
      '${(s ~/ 60).toString().padLeft(2, '0')}:${(s % 60).toString().padLeft(2, '0')}';
}
