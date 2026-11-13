import 'dart:io';

import 'package:file_picker/file_picker.dart';
import 'package:flutter/material.dart';
import 'package:path_provider/path_provider.dart';

import '../diag/boot_log.dart';
import '../diag/dev_panel.dart';
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

  void _toast(String msg) {
    if (!mounted) return;
    ScaffoldMessenger.of(context).showSnackBar(
      SnackBar(content: Text(msg), duration: const Duration(seconds: 3)),
    );
  }

  /// 导入诊断：把每一步结果追加到公共目录（老板能直接拿、我也能读）。
  /// 写不进去本身也是信息（= 没有「所有文件访问」权限）。
  static const String _diagPath = '/storage/emulated/0/yieyu/import_diag.txt';

  static Future<void> _diag(String line) async {
    try {
      final f = File(_diagPath);
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
      await _diag('importScript name=${f.name} size=${bytes.length} rc=$rc');
      if (rc != 0) {
        await BootLog.instance.write('importScript 失败 rc=$rc name=${f.name} size=${bytes.length}');
      }
      setState(() => _busy = false);
      switch (rc) {
        case 0:
          _toast('脚本导入成功，已就绪');
        case -2:
          _toast('该脚本包含不被允许的操作，已拒绝');
        case -4:
          _toast('脚本太大了（上限 512KB）');
        default:
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
        final pub = Directory('/storage/emulated/0/yieyu');
        final target = await pub.exists()
            ? pub.path
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
            child: ListTile(
              leading: const Icon(Icons.description_outlined),
              title: const Text('导出诊断日志'),
              subtitle: const Text('遇到问题时导出，便于排查'),
              onTap: _busy ? null : _export,
            ),
          ),
        ],
      ),
    );
  }
}
