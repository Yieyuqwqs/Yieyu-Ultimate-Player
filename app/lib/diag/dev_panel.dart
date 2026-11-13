import 'dart:async';

import 'package:flutter/material.dart';

import '../jsb/jsb_client.dart';
import 'boot_log.dart';

/// 【诊断】开发者面板 —— **只能从设置页标题连点 5 次进来**。
///
/// 这里的内容是给开发看的：播放链路试听、位置/采样率、启动日志原文。
/// 正常界面永远不许出现这些（docs/BLUEPRINT.md §0 铁律 8：UI 绝对静默）。
class DevPanel extends StatefulWidget {
  const DevPanel({super.key});

  @override
  State<DevPanel> createState() => _DevPanelState();
}

class _DevPanelState extends State<DevPanel> {
  final TextEditingController _urlCtrl = TextEditingController();
  StreamSubscription<YieyuSnapshot>? _sub;
  YieyuSnapshot _snap = YieyuSnapshot.zero();
  String _hint = '';
  double _volume = 1.0;
  String _bootLog = '';

  @override
  void initState() {
    super.initState();
    _sub = JsbClient.instance.snapshots.listen((s) {
      if (mounted) setState(() => _snap = s);
    });
    _loadLog();
  }

  Future<void> _loadLog() async {
    final s = await BootLog.instance.read();
    if (mounted) setState(() => _bootLog = s);
  }

  @override
  void dispose() {
    _sub?.cancel();
    _urlCtrl.dispose();
    super.dispose();
  }

  void _play() {
    final url = _urlCtrl.text.trim();
    if (url.isEmpty) {
      setState(() => _hint = '先填一个音频直链或本地路径');
      return;
    }
    final rc = JsbClient.instance.playUrl(url);
    BootLog.instance.write('devpanel playUrl rc=$rc');
    setState(() => _hint = rc == 0 ? '已提交，正在起播…' : '未能受理（$rc）');
  }

  String _t(int ms) {
    final s = (ms / 1000).floor();
    return '${(s ~/ 60).toString().padLeft(2, '0')}:${(s % 60).toString().padLeft(2, '0')}';
  }

  @override
  Widget build(BuildContext context) {
    final dur = _snap.durationMs;
    final pos = _snap.positionMs;
    return Scaffold(
      appBar: AppBar(title: const Text('开发者面板')),
      body: ListView(
        padding: const EdgeInsets.all(16),
        children: [
          Card(
            child: Padding(
              padding: const EdgeInsets.all(12),
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  const Text('播放试听', style: TextStyle(fontWeight: FontWeight.bold)),
                  const SizedBox(height: 8),
                  TextField(
                    controller: _urlCtrl,
                    decoration: const InputDecoration(
                      hintText: '音频直链（https://…）或本地路径',
                      isDense: true,
                      border: OutlineInputBorder(),
                    ),
                  ),
                  const SizedBox(height: 8),
                  Wrap(
                    spacing: 8,
                    runSpacing: 8,
                    children: [
                      FilledButton(onPressed: _play, child: const Text('播放')),
                      OutlinedButton(
                        onPressed: () => JsbClient.instance.pause(),
                        child: const Text('暂停'),
                      ),
                      OutlinedButton(
                        onPressed: () => JsbClient.instance.resume(),
                        child: const Text('继续'),
                      ),
                      OutlinedButton(
                        onPressed: () => JsbClient.instance.stop(),
                        child: const Text('停止'),
                      ),
                      OutlinedButton(
                        onPressed: () => JsbClient.instance.seekMs((pos - 10000).clamp(0, 1 << 40).toInt()),
                        child: const Text('−10s'),
                      ),
                      OutlinedButton(
                        onPressed: () => JsbClient.instance.seekMs(pos + 10000),
                        child: const Text('+10s'),
                      ),
                    ],
                  ),
                  const SizedBox(height: 12),
                  Row(
                    children: [
                      const Text('音量', style: TextStyle(fontSize: 12)),
                      Expanded(
                        child: Slider(
                          value: _volume,
                          onChanged: (v) {
                            setState(() => _volume = v);
                            JsbClient.instance.setVolume(v);
                          },
                        ),
                      ),
                      Text('${(_volume * 100).round()}%', style: const TextStyle(fontSize: 12)),
                    ],
                  ),
                  const SizedBox(height: 8),
                  LinearProgressIndicator(value: dur <= 0 ? 0 : (pos / dur).clamp(0.0, 1.0)),
                  const SizedBox(height: 8),
                  Text(
                    '${_snap.playing ? '播放中' : '未播放'} · ${_t(pos)} / ${_t(dur)} · '
                    '${_snap.sampleRate} Hz · 峰值 ${_snap.peakL.toStringAsFixed(2)}',
                    style: const TextStyle(fontSize: 11, color: Colors.white54),
                  ),
                  if (_hint.isNotEmpty)
                    Padding(
                      padding: const EdgeInsets.only(top: 6),
                      child: Text(_hint, style: const TextStyle(fontSize: 11, color: Colors.white38)),
                    ),
                ],
              ),
            ),
          ),
          Card(
            child: ListTile(
              leading: const Icon(Icons.description_outlined),
              title: const Text('启动日志'),
              subtitle: const Text('点开看原文（含原生库加载结果）'),
              onTap: () {
                _loadLog();
                showDialog<void>(
                  context: context,
                  builder: (ctx) => AlertDialog(
                    title: const Text('启动日志'),
                    content: SingleChildScrollView(
                      child: SelectableText(_bootLog, style: const TextStyle(fontSize: 11)),
                    ),
                    actions: [
                      TextButton(onPressed: () => Navigator.pop(ctx), child: const Text('关闭')),
                    ],
                  ),
                );
              },
            ),
          ),
        ],
      ),
    );
  }
}
