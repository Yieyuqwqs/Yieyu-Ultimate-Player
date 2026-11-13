import 'dart:io';
import 'dart:typed_data';

import 'package:file_picker/file_picker.dart';
import 'package:flutter/material.dart';

import '../jsb/jsb_client.dart';

/// 【模块 1】设置页 —— 两个真功能：导入 Lua 脚本、导出诊断日志。
///
/// ★ UI 静默铁律：这里**不许**显示路径、异常、堆栈、模块名。
///   导入失败只说"脚本导入失败"，具体原因进黑匣子。
class SettingsPage extends StatefulWidget {
  const SettingsPage({super.key});

  @override
  State<SettingsPage> createState() => _SettingsPageState();
}

class _SettingsPageState extends State<SettingsPage> {
  bool _busy = false;

  void _toast(String msg) {
    if (!mounted) return;
    ScaffoldMessenger.of(context).showSnackBar(
      SnackBar(content: Text(msg), duration: const Duration(seconds: 3)),
    );
  }

  /// 调起系统 SAF 选择器 → 读字节 → 喂给底层沙箱
  Future<void> _importLua() async {
    if (_busy) return;
    setState(() => _busy = true);
    try {
      final picked = await FilePicker.platform.pickFiles(
        type: FileType.any,          // 有些 ROM 不认 .lua 这个 MIME，放开类型更稳
        withData: true,              // 直接拿字节，避免再要存储权限
        allowMultiple: false,
      );
      if (picked == null || picked.files.isEmpty) {
        setState(() => _busy = false);
        return;                       // 用户取消：什么都不说
      }
      final f = picked.files.single;
      if (!f.name.toLowerCase().endsWith('.lua')) {
        setState(() => _busy = false);
        _toast('请选择 .lua 脚本文件');
        return;
      }
      Uint8List? bytes = f.bytes;
      if (bytes == null && f.path != null) {
        bytes = await File(f.path!).readAsBytes();
      }
      if (bytes == null) {
        setState(() => _busy = false);
        _toast('读取失败，请重试');
        return;
      }

      final rc = JsbClient.instance.importScript(f.name, bytes);
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
    } catch (_) {
      setState(() => _busy = false);
      _toast('脚本导入失败，请重试');   // 异常对象绝不外露
    }
  }

  Future<void> _export() async {
    if (_busy) return;
    setState(() => _busy = true);
    final ok = JsbClient.instance.exportLogs() != null;
    if (!mounted) return;
    setState(() => _busy = false);
    _toast(ok ? '已导出，可在文件管理器里查看' : '导出没成功，请重试');
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(title: const Text('设置')),
      body: ListView(
        padding: const EdgeInsets.all(16),
        children: [
          // 主按钮：导入脚本（热更唯一入口）
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
