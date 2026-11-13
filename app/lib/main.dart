import 'dart:async';
import 'dart:ffi' show Abi;

import 'package:flutter/material.dart';
import 'package:path_provider/path_provider.dart';

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
      if (rc != 0) _degraded = true;      // rc 只进日志，不展示
      JsbClient.instance.startPump();
    } catch (_) {
      _degraded = true;                  // 异常对象绝不外露
    } finally {
      if (mounted) setState(() => _ready = true);
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
