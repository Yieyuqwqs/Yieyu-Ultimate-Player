import 'dart:async';
import 'dart:ffi' show Abi;

import 'package:flutter/material.dart';
import 'package:flutter/services.dart' show AssetManifest, rootBundle;
import 'package:path_provider/path_provider.dart';

import 'diag/boot_log.dart';
import 'jsb/jsb_client.dart';
import 'shell/app_router.dart';

/// 【模块 1】入口。
///
/// ★ UI 静默铁律（docs/BLUEPRINT.md §日志规范 1）：
///   底层无论 C++ 崩溃、Go 超时还是 Rust 验签失败，界面**绝不**显示技术细节。
///   这里只在启动失败时给一句"出了点小状况"，具体原因全部进黑匣子日志。
void main() {
  WidgetsFlutterBinding.ensureInitialized();
  runApp(const _Bootstrapper());
}

class _Bootstrapper extends StatefulWidget {
  const _Bootstrapper();
  @override
  State<_Bootstrapper> createState() => _BootstrapperState();
}

class _BootstrapperState extends State<_Bootstrapper> {
  bool _ready = false;
  bool _degraded = false;

  @override
  void initState() {
    super.initState();
    _boot();
  }

  Future<void> _boot() async {
    try {
      final dir = await getApplicationSupportDirectory();
      final rc = JsbClient.instance.init(dir.path, Abi.current().toString());
      if (rc != 0) {
        _degraded = true;
        BootLog.instance.write('Yieyu_Init 返回 $rc');
      } else {
        JsbClient.instance.startPump();
        await _verifyIntegrity();
        await _installBuiltinScripts();
        await _installBuiltinPlugins();
      }
      BootLog.instance.write('boot ok, abi=${Abi.current()}');
    } catch (e) {
      _degraded = true;                  // 异常对象绝不外露给界面
      BootLog.instance.write('原生库加载失败: $e');
    } finally {
      if (mounted) setState(() => _ready = true);
    }
  }

  /// [7] 随包脚本：脚本目录里一个都没有时，把 assets/scripts/*.lua 导进去。
  /// 这样新装 App 也有一个能用的音源，不用先去找 .lua 文件手动导入。
  Future<void> _installBuiltinScripts() async {
    try {
      final r = JsbClient.instance.reloadScripts();
      if (r != null && r.$1 > 0) return;              // 已经有脚本了，不动
      for (final name in const <String>['wy.lua']) {
        try {
          final bytes = await rootBundle.load('assets/scripts/$name');
          JsbClient.instance.importScript(name, bytes.buffer.asUint8List());
        } catch (_) {
          // 资产缺失（本地 dev）不是错误
        }
      }
      JsbClient.instance.reloadScripts();
    } catch (_) {}
  }

  /// [8] 随包插件：把 assets/plugins/*.wasm 交给原生（它会落盘到 <home>/plugins 并加载）。
  /// 没有插件也是正常情况（插件是可选能力），所以这里一律静默。
  Future<void> _installBuiltinPlugins() async {
    try {
      final manifest = await AssetManifest.loadFromAssetBundle(rootBundle);
      final keys = manifest.listAssets().where((k) => k.startsWith('assets/plugins/') && k.endsWith('.ypfx'));
      for (final key in keys) {
        final data = await rootBundle.load(key);
        final name = key.split('/').last;
        JsbClient.instance.importPlugin(name, data.buffer.asUint8List());
      }
    } catch (_) {}
  }

  /// [11] 防篡改：资产里的期望摘要 vs 磁盘上 so 的真实摘要。
  /// 不一致不打扰用户（界面静默），只进黑匣子与开发者面板。
  Future<void> _verifyIntegrity() async {
    try {
      final want = (await rootBundle.loadString('assets/integrity/libyieyu_core.sha256')).trim();
      if (want.length != 64) return;
      final rc = JsbClient.instance.integrityCheck(want);
      BootLog.instance.write('[11] integrity rc=$rc');
    } catch (e) {
      // 资产缺失（本地 dev 构建）不是错误，忽略
    }
  }

  @override
  Widget build(BuildContext context) {
    if (!_ready) {
      return const MaterialApp(
        debugShowCheckedModeBanner: false,
        home: Scaffold(body: Center(child: CircularProgressIndicator())),
      );
    }
    return YieyuApp(degraded: _degraded);
  }
}

class YieyuApp extends StatefulWidget {
  const YieyuApp({super.key, this.degraded = false});
  final bool degraded;

  @override
  State<YieyuApp> createState() => _YieyuAppState();
}

class _YieyuAppState extends State<YieyuApp> {
  final _scaffoldKey = GlobalKey<ScaffoldMessengerState>();

  @override
  void initState() {
    super.initState();
    if (widget.degraded) {
      // 只说人话，不解释为什么（原因在黑匣子里）
      WidgetsBinding.instance.addPostFrameCallback((_) {
        _scaffoldKey.currentState?.showSnackBar(
          const SnackBar(
            content: Text('出了点小状况，部分功能暂时不可用'),
            duration: Duration(seconds: 4),
          ),
        );
      });
    }
  }

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      scaffoldMessengerKey: _scaffoldKey,
      title: 'Yieyu Ultimate',
      debugShowCheckedModeBanner: false,
      theme: ThemeData(
        colorScheme: ColorScheme.fromSeed(
          seedColor: const Color(0xFF6E4BFF),
          brightness: Brightness.dark,
        ),
        useMaterial3: true,
      ),
      onGenerateRoute: AppRouter.generate,
      initialRoute: AppRouter.home,
    );
  }
}
