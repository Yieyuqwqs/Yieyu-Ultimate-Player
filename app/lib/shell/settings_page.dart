import 'dart:io';

import 'package:file_picker/file_picker.dart';
import 'package:flutter/material.dart';
import 'package:path_provider/path_provider.dart';

import '../diag/boot_log.dart';
import '../diag/dev_panel.dart';
import '../diag/modules_page.dart';
import '../jsb/jsb_client.dart';

/// 【模块 1】设置页：导入 Lua 脚本 / 导出诊断日志。
///
/// ★ UI 静默铁律：正常路径下**不显示**任何路径、异常、堆栈、模块名。
///   唯一的例外是「隐秘查错入口」：连续点标题 5 次，才把启动日志原文摊出来（给开发看）。
class SettingsPage extends StatefulWidget {
  const SettingsPage({super.key});

  @override
  State<SettingsPage> createState() => _SettingsPageState();
}

class _SettingsPageState extends State<SettingsPage> {
  bool _busy = false;
  int _titleTaps = 0;
  // 输出采样率（模块 14）：0=自动（PCM 跟源、DSD→192k）
  int _rate = 0;
  bool _dsp = false;

  void _toast(String msg) {
    if (!mounted) return;
    ScaffoldMessenger.of(context).showSnackBar(
      SnackBar(content: Text(msg), duration: const Duration(seconds: 3)),
    );
  }

  /// 导入诊断：把每一步结果追加到公共目录（老板能直接拿、我也能读）。
  /// 写不进去本身也是信息（= 没有「所有文件访问」权限）。
  /// 诊断落盘位置由**原生**决定（app_paths），Dart 里不再出现 /storage/… 这种硬编码
  static Future<void> _diag(String line) async {
    final dir = JsbClient.instance.logDir();
    if (dir.isEmpty) return;                 // 原生还没起来：静默放弃
    try {
      final f = File('$dir/import_diag.txt');
      await f.parent.create(recursive: true);
      await f.writeAsString('${DateTime.now().toIso8601String()} | $line\n',
          mode: FileMode.append, flush: true);
    } catch (_) {}
  }

  Future<void> _importLua() async {
    if (_busy) return;
    setState(() => _busy = true);
    try {
      final picked = await FilePicker.platform.pickFiles(
        type: FileType.any,
        withData: true,
        allowMultiple: false,
      );
      if (picked == null || picked.files.isEmpty) {
        await _diag('选择器返回空（可能被取消，或 SAF 拿不到该文件）');
        setState(() => _busy = false);
        return;
      }
      final f = picked.files.single;
      if (!f.name.toLowerCase().endsWith('.lua')) {
        await _diag('名字不是 .lua：${f.name}');
        setState(() => _busy = false);
        _toast('请选择 .lua 脚本文件');
        return;
      }
      var bytes = f.bytes;
      if (bytes == null && f.path != null) {
        bytes = await File(f.path!).readAsBytes();
      }
      if (bytes == null) {
        await _diag('读到空内容：${f.name} path=${f.path}');
        setState(() => _busy = false);
        _toast('读取失败，请重试');
        return;
      }

      final rc = JsbClient.instance.importScript(f.name, bytes) ?? -1;
      final why = rc == 0 ? '' : JsbClient.instance.lastError();
      await _diag('importScript name=${f.name} size=${bytes.length} rc=$rc why=$why');
      if (rc != 0) {
        await BootLog.instance.write('importScript 失败 rc=$rc name=${f.name} size=${bytes.length}');
      }
      setState(() => _busy = false);
      // ★ 这里原来是 switch 且四个 case 都没 break：Dart 会直落，
      //   成功也会连着弹到最后一句"脚本导入失败"，看起来永远导不进去。改成互斥分支。
      if (rc == 0) {
        _toast('脚本导入成功，已就绪');
      } else if (rc == -2) {
        _toast('该脚本包含不被允许的操作，已拒绝');
      } else if (rc == -4) {
        _toast('脚本太大了（上限 512KB）');
      } else {
        _toast('脚本导入失败，请重试');
      }
    } catch (e) {
      await _diag('importScript 异常: $e');
      await BootLog.instance.write('importScript 异常: $e');
      if (mounted) setState(() => _busy = false);
      _toast('脚本导入失败，请重试');
    }
  }

  Future<void> _export() async {
    if (_busy) return;
    setState(() => _busy = true);

    // ① 核心在 → 导出黑匣子（真日志）
    final dir = JsbClient.instance.exportLogs();
    var ok = dir != null;

    // ② 核心不在（启动就降级）→ 至少把 Flutter 侧启动日志导出
    if (!ok) {
      try {
        final dir = JsbClient.instance.logDir();
        final target = dir.isNotEmpty
            ? dir
            : (await getExternalStorageDirectory())?.path ??
                (await getApplicationSupportDirectory()).path;
        ok = await BootLog.instance.exportTo(target);
      } catch (_) {
        ok = false;
      }
    }

    if (!mounted) return;
    setState(() => _busy = false);
    _toast(ok ? '已导出，可在文件管理器里查看' : '导出没成功，请重试');
  }

  Future<void> _secretDiag() async {
    // 隐秘查错入口：连点标题 5 次 → 开发者面板（播放试听 + 启动日志）
    await Navigator.of(context).push(
      MaterialPageRoute(builder: (_) => const DevPanel()),
    );
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: GestureDetector(
          onTap: () {
            _titleTaps++;
            if (_titleTaps >= 5) {
              _titleTaps = 0;
              _secretDiag();     // 隐秘查错入口
            }
          },
          child: const Text('设置'),
        ),
      ),
      body: ListView(
        padding: const EdgeInsets.all(16),
        children: [
          SizedBox(
            width: double.infinity,
            height: 52,
            child: FilledButton.icon(
              onPressed: _busy ? null : _importLua,
              icon: _busy
                  ? const SizedBox(
                      width: 18, height: 18, child: CircularProgressIndicator(strokeWidth: 2))
                  : const Icon(Icons.file_upload_outlined),
              label: const Text('导入 Lua 脚本', style: TextStyle(fontSize: 16)),
            ),
          ),
          const SizedBox(height: 8),
          const Text(
            '选择 .lua 音源脚本，导入后立即生效，无需重新安装',
            style: TextStyle(fontSize: 12, color: Colors.white38),
          ),
          const SizedBox(height: 24),
          Card(
            child: Column(
              children: [
                ListTile(
                  leading: const Icon(Icons.graphic_eq),
                  title: const Text('输出采样率'),
                  subtitle: Text(_rate == 0 ? '自动（跟随音源）' : '${_rate ~/ 1000} kHz（模块 14 升频）'),
                  trailing: DropdownButton<int>(
                    value: _rate,
                    underline: const SizedBox.shrink(),
                    items: const [
                      DropdownMenuItem(value: 0, child: Text('自动')),
                      DropdownMenuItem(value: 48000, child: Text('48k')),
                      DropdownMenuItem(value: 96000, child: Text('96k')),
                      DropdownMenuItem(value: 192000, child: Text('192k')),
                    ],
                    onChanged: (v) {
                      if (v == null) return;
                      setState(() => _rate = v);
                      JsbClient.instance.setRate(v);   // [14]
                      _toast(v == 0 ? '下次起播：跟随音源' : '下次起播：升频到 ${v ~/ 1000} kHz');
                    },
                  ),
                ),
                SwitchListTile(
                  secondary: const Icon(Icons.equalizer),
                  title: const Text('音效链'),
                  subtitle: const Text('31 段 EQ + 胆机/黑胶 + 8D 空间化'),
                  value: _dsp,
                  onChanged: (v) {
                    setState(() => _dsp = v);
                    JsbClient.instance.setDsp(v);    // [17]
                  },
                ),
              ],
            ),
          ),
          const SizedBox(height: 8),
          Card(
            child: ListTile(
              leading: const Icon(Icons.description_outlined),
              title: const Text('导出诊断日志'),
              subtitle: const Text('遇到问题时导出，便于排查'),
              onTap: _busy ? null : _export,
            ),
          ),
          Card(
            child: ListTile(
              leading: const Icon(Icons.grid_view_rounded),
              title: const Text('模块自检'),
              subtitle: const Text('24 个模块当前是不是真的在跑'),
              onTap: () => Navigator.of(context).push(
                MaterialPageRoute(builder: (_) => const ModulesPage()),
              ),
            ),
          ),
        ],
      ),
    );
  }
}
