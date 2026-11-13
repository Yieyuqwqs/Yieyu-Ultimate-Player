import 'dart:async';
import 'dart:io';

import 'dart:math' as math;
import 'dart:typed_data';

import 'package:flutter/material.dart';
import 'package:path_provider/path_provider.dart';

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
  String _stats = '';
  Timer? _statsTimer;
  double _volume = 1.0;
  String _bootLog = '';

  @override
  void initState() {
    super.initState();
    _sub = JsbClient.instance.snapshots.listen((s) {
      if (mounted) setState(() => _snap = s);
    });
    _loadLog();
    _statsTimer = Timer.periodic(const Duration(milliseconds: 700), (_) {
      final line = JsbClient.instance.statsLine();
      if (mounted && line.isNotEmpty && line != _stats) setState(() => _stats = line);
    });
  }

  Future<void> _loadLog() async {
    final s = await BootLog.instance.read();
    if (mounted) setState(() => _bootLog = s);
  }

  @override
  void dispose() {
    _statsTimer?.cancel();
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

  /// 本地测试音：不联网也能验证 13→15 整条链路（解码 + AAudio）。
  /// 44.1kHz / 16bit / 立体声 / 3 秒 440Hz，带 20ms 淡入淡出（防爆音）。
  Future<void> _playTestTone() async {
    setState(() => _hint = '正在生成测试音…');
    try {
      final dir = await getApplicationSupportDirectory();
      final f = File('${dir.path}/test440.wav');
      if (!await f.exists()) await f.writeAsBytes(_wavTone(440.0, 3.0, 44100));
      final rc = JsbClient.instance.playUrl(f.path);
      setState(() => _hint = rc == 0 ? '已提交测试音（3 秒 440Hz）' : '未能受理（$rc）');
    } catch (_) {
      setState(() => _hint = '测试音写盘失败');
    }
  }

  Uint8List _wavTone(double freq, double secs, int rate) {
    const int ch = 2;
    final int frames = (rate * secs).round();
    final data = Uint8List(frames * ch * 2);
    final bd = ByteData.view(data.buffer);
    const double fade = 0.02;
    for (var i = 0; i < frames; i++) {
      final t = i / rate;
      var env = 1.0;
      if (t < fade) env = t / fade;
      if (t > secs - fade) env = (secs - t) / fade;
      final v = (0.28 * env * math.sin(2 * math.pi * freq * t) * 32767).round().clamp(-32767, 32767).toInt();
      bd.setInt16(i * 4, v, Endian.little);
      bd.setInt16(i * 4 + 2, v, Endian.little);
    }
    final h = BytesBuilder();
    void u32(int v) => h.add(<int>[v & 0xff, (v >> 8) & 0xff, (v >> 16) & 0xff, (v >> 24) & 0xff]);
    void u16(int v) => h.add(<int>[v & 0xff, (v >> 8) & 0xff]);
    h.add('RIFF'.codeUnits);
    u32(36 + data.length);
    h.add('WAVE'.codeUnits);
    h.add('fmt '.codeUnits);
    u32(16);
    u16(1);
    u16(ch);
    u32(rate);
    u32(rate * ch * 2);
    u16(ch * 2);
    u16(16);
    h.add('data'.codeUnits);
    u32(data.length);
    return Uint8List.fromList(<int>[...h.toBytes(), ...data]);
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
                      OutlinedButton(
                        onPressed: _playTestTone,
                        child: const Text('本地测试音'),
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
                  if (_stats.isNotEmpty)
                    Padding(
                      padding: const EdgeInsets.only(top: 6),
                      child: SelectableText(_stats,
                          style: const TextStyle(fontSize: 10, color: Colors.white38)),
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
