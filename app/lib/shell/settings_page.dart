import 'package:flutter/material.dart';

import '../jsb/jsb_client.dart';

/// 【模块 1】设置页 —— 只有一个功能：导出诊断日志。
///
/// ★ UI 静默铁律：这里**不许**显示任何路径、异常、堆栈、模块名。
///   导出成功就说"已导出"，失败就说"请重试"。真原因去黑匣子里看。
class SettingsPage extends StatefulWidget {
  const SettingsPage({super.key});

  @override
  State<SettingsPage> createState() => _SettingsPageState();
}

class _SettingsPageState extends State<SettingsPage> {
  bool _busy = false;

  Future<void> _export() async {
    if (_busy) return;
    setState(() => _busy = true);
    final ok = JsbClient.instance.exportLogs() != null;
    if (!mounted) return;
    setState(() => _busy = false);
    ScaffoldMessenger.of(context).showSnackBar(
      SnackBar(
        content: Text(ok ? '已导出，可在文件管理器里查看' : '导出没成功，请重试'),
        duration: const Duration(seconds: 3),
      ),
    );
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(title: const Text('设置')),
      body: ListView(
        children: [
          ListTile(
            leading: const Icon(Icons.description_outlined),
            title: const Text('导出诊断日志'),
            subtitle: const Text('遇到问题时导出，便于排查'),
            trailing: _busy
                ? const SizedBox(
                    width: 18, height: 18, child: CircularProgressIndicator(strokeWidth: 2))
                : null,
            onTap: _export,
          ),
          ListTile(
            leading: const Icon(Icons.extension_outlined),
            title: const Text('重新加载音源脚本'),
            subtitle: const Text('把 .lua 放进 yieyu/scripts/ 后点这里，无需重装'),
            onTap: () {
              final r = JsbClient.instance.reloadScripts();
              final msg = r == null
                  ? '加载失败，请重试'
                  : (r.$1 == 0 ? '没找到脚本' : '已加载 ${r.$1} 个脚本');
              ScaffoldMessenger.of(context).showSnackBar(
                SnackBar(content: Text(msg), duration: const Duration(seconds: 3)),
              );
            },
          ),
          const Divider(),
          const Padding(
            padding: EdgeInsets.all(16),
            child: Text(
              '应用会静默记录运行错误，正常使用不会产生多余写入。',
              style: TextStyle(fontSize: 12, color: Colors.white38),
            ),
          ),
        ],
      ),
    );
  }
}
