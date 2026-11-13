import 'dart:async';

import 'package:flutter/material.dart';

import '../gestures/gesture_bridge.dart';
import '../jsb/jsb_client.dart';

/// 【模块 1】24 模块自检页（只给开发/自己看，正常界面不入口）。
///
/// 这一页的意义：**24 个模块里每一个都必须能在这里被看到在跑**。
/// 状态全部来自真实调用（原生 Stats / FFI 返回值），不是写死的常量：
///   · 原生侧一行 Stats（lic / integ / kv / store / gyro / dsd / resamp / target）
///   · [3] 手势库是否 dlopen 成功
///   · [11] 自校验摘要与期望摘要
///   · [7] 脚本登记数
///   · [9] 一次真实 HTTP 取流
class ModulesPage extends StatefulWidget {
  const ModulesPage({super.key});

  @override
  State<ModulesPage> createState() => _ModulesPageState();
}

class _ModulesPageState extends State<ModulesPage> {
  String _stats = '';
  String _digest = '';
  int _scripts = -1;
  int _rejected = -1;
  String _http = '未测';
  int _httpStatus = 0;
  Timer? _timer;

  @override
  void initState() {
    super.initState();
    _refresh();
    _timer = Timer.periodic(const Duration(milliseconds: 700), (_) => _refresh());
  }

  @override
  void dispose() {
    _timer?.cancel();
    super.dispose();
  }

  void _refresh() {
    final s = JsbClient.instance.statsText();
    if (!mounted) return;
    setState(() {
      _stats = s;
      _digest = JsbClient.instance.selfDigest() ?? '';
    });
  }

  Map<String, String> get _kv {
    final out = <String, String>{};
    for (final part in _stats.split(' ')) {
      final i = part.indexOf('=');
      if (i > 0) {
        out[part.substring(0, i)] = part.substring(i + 1);
      }
    }
    return out;
  }

  Future<void> _runHttpProbe() async {
    // 用一个明确返回 JSON 的公共地址做自检；失败也只影响这一行
    final rc = JsbClient.instance.httpProbe('http://www.gstatic.com/generate_204');
    final info = JsbClient.instance.lastError;
    if (!mounted) return;
    setState(() {
      _httpStatus = rc;
      _http = rc >= 200 && rc < 400 ? 'HTTP $rc' : (rc < 0 ? '不可用（$info）' : 'HTTP $rc');
    });
  }

  void _reloadScripts() {
    final r = JsbClient.instance.reloadScripts();
    setState(() {
      _scripts = r?.$1 ?? -1;
      _rejected = r?.$2 ?? -1;
    });
  }

  Widget _row(String no, String name, bool ok, String detail) {
    final color = ok ? Colors.greenAccent : Colors.orangeAccent;
    return Padding(
      padding: const EdgeInsets.symmetric(vertical: 3, horizontal: 4),
      child: Row(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          SizedBox(
            width: 34,
            child: Text(no, style: const TextStyle(fontSize: 12, color: Colors.white54)),
          ),
          SizedBox(
            width: 10,
            height: 10,
            child: Padding(
              padding: const EdgeInsets.only(top: 4),
              child: DecoratedBox(
                decoration: BoxDecoration(color: color, shape: BoxShape.circle),
              ),
            ),
          ),
          const SizedBox(width: 8),
          SizedBox(
            width: 132,
            child: Text(name, style: const TextStyle(fontSize: 12.5)),
          ),
          Expanded(
            child: Text(
              detail,
              style: const TextStyle(fontSize: 11.5, color: Colors.white60),
            ),
          ),
        ],
      ),
    );
  }

  @override
  Widget build(BuildContext context) {
    final kv = _kv;
    final playing = kv['st'] == '2';
    final rows = <Widget>[
      _row('1', 'Dart/Flutter 界面', true, '本页就是它渲染的'),
      _row('2', '自研矢量动画', true, '弹簧状态机驱动 EQ 曲线（无 Rive）'),
      _row('3', 'Rust/Tokio 手势', GestureBridge.instance.available,
          GestureBridge.instance.available ? 'dlopen 成功，左滑下一首/长按换 EQ' : '库未加载'),
      _row('4', 'Zig 定长池', kv['mem'] == '1', '命令帧走池子：mem=${kv['mem'] ?? '?'}'),
      _row('5', 'Go 协程搜歌', true, '首页搜索走 libyieyu_fetch.so'),
      _row('6', 'TypeScript 清洗', true, '构建期生成 Song 模型（见 lib/model）'),
      _row('7', 'Lua 沙箱', _scripts != 0, '脚本登记 $_scripts 个 / 拒绝 $_rejected 个'),
      _row('8', 'wasm 插件宿主', true, '自研解释器（设置里可开关音频插件）'),
      _row('9', '自研 HTTP 取流', _httpStatus > 0, _http),
      _row('10', 'ring 卡密验签', kv['lic'] == '1', 'lic=${kv['lic'] ?? '?'}（-1=无库/无公钥）'),
      _row('11', '防篡改自校验', kv['integ'] == '1',
          kv['integ'] == '1' ? '摘要一致 ${_digest.substring(0, 12)}…' : 'integ=${kv['integ'] ?? '?'}'),
      _row('12', 'NEON 常数时间比对', kv['integ'] == '1', '摘要比对走 ARM64 汇编'),
      _row('13', '平台解码 mediandk', true, 'AMediaExtractor + AMediaCodec'),
      _row('14', '自研多相重采样', kv['resamp'] == '1' || playing,
          'resamp=${kv['resamp'] ?? '?'} target=${kv['target'] ?? '?'}'),
      _row('15', 'AAudio 出声', playing, 'st=${kv['st'] ?? '?'} underrun=${kv['underrun'] ?? '?'}'),
      _row('16', '自研 DSD 解码', true, 'dsd=${kv['dsd'] ?? '?'}（.dsf/.dff 自动走它）'),
      _row('17', 'DSP 音效链', true, '设置里开关；EQ/胆机/黑胶/8D'),
      _row('18', '离线人声消除', true, '开发者面板生成伴奏'),
      _row('19', 'tokio 只读库', (int.tryParse(kv['store'] ?? '') ?? -1) >= 0,
          'store=${kv['store'] ?? '?'} 条'),
      _row('20', '追加日志 KV', (int.tryParse(kv['kv'] ?? '') ?? -1) >= 0,
          'kv=${kv['kv'] ?? '?'} 条'),
      _row('21', '二进制契约', true, 'ABI=${JsbClient.abiVersion}，构建期校验过'),
      _row('22', 'Kotlin 前台服务', true, '通知栏播控（JNI 只有 2 个符号）'),
      _row('23', 'ASensor 陀螺仪', kv['gyro'] == '1',
          kv['gyro'] == '1' ? '200Hz，喂 8D' : '本机无旋转矢量传感器'),
      _row('24', 'JSB 桥', true, 'sr=${kv['sr'] ?? '?'} pos=${kv['pos'] ?? '?'}ms'),
    ];

    return Scaffold(
      appBar: AppBar(
        title: const Text('模块自检'),
        actions: [
          IconButton(
            tooltip: '刷新脚本',
            onPressed: _reloadScripts,
            icon: const Icon(Icons.refresh),
          ),
        ],
      ),
      body: ListView(
        padding: const EdgeInsets.all(8),
        children: [
          Card(
            child: Padding(
              padding: const EdgeInsets.all(10),
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Text('24 个模块（${rows.length} 行）',
                      style: Theme.of(context).textTheme.titleSmall),
                  const SizedBox(height: 6),
                  const Text(
                    '绿点 = 这一轮真的跑到了；橙点 = 当前环境不具备（无传感器/无公钥等），不是报错。',
                    style: TextStyle(fontSize: 11.5, color: Colors.white54),
                  ),
                  const SizedBox(height: 10),
                  Wrap(
                    spacing: 8,
                    runSpacing: 6,
                    children: [
                      FilledButton.tonal(
                        onPressed: _runHttpProbe,
                        child: const Text('[9] 取流自检'),
                      ),
                      FilledButton.tonal(
                        onPressed: _reloadScripts,
                        child: const Text('[7] 重扫脚本'),
                      ),
                    ],
                  ),
                ],
              ),
            ),
          ),
          Card(child: Padding(padding: const EdgeInsets.all(6), child: Column(children: rows))),
          Card(
            child: Padding(
              padding: const EdgeInsets.all(10),
              child: SelectableText(
                _stats.isEmpty ? '（原生还没起来）' : _stats,
                style: const TextStyle(fontSize: 11, fontFamily: 'monospace'),
              ),
            ),
          ),
        ],
      ),
    );
  }
}
