import 'dart:ffi' show Abi;

import 'package:flutter/material.dart';
import 'package:path_provider/path_provider.dart';

import 'jsb/jsb_client.dart';
import 'shell/app_router.dart';

/// 【模块 1】入口。
///
/// 保命设计：本地原生库（M24 JSB）加载失败时，App **仍然要起来**，
/// 只是顶部挂一条红色诊断条 —— 骨架期每天在换 .so，不能因为一个符号没导就白屏。
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
  String? _err;
  bool _ready = false;

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
        _err = 'Yieyu_Init 返回 $rc（核心内部故障）';
      } else {
        JsbClient.instance.startPump();
      }
    } catch (e) {
      _err = '原生库未就位：$e';
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
    return YieyuApp(bootError: _err);
  }
}

class YieyuApp extends StatelessWidget {
  const YieyuApp({super.key, this.bootError});
  final String? bootError;

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
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
      builder: (ctx, child) => bootError == null
          ? (child ?? const SizedBox())
          : Column(children: [
              Material(
                color: Colors.red.shade900,
                child: SafeArea(
                  bottom: false,
                  child: Padding(
                    padding: const EdgeInsets.all(8),
                    child: Text(
                      '原生层降级运行：$bootError',
                      style: const TextStyle(fontSize: 11, color: Colors.white),
                    ),
                  ),
                ),
              ),
              Expanded(child: child ?? const SizedBox()),
            ]),
    );
  }
}
