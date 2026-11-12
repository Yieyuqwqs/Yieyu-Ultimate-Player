import 'dart:io';

import 'package:flutter/material.dart';
import 'package:path_provider/path_provider.dart';

import 'jsb/jsb_client.dart';
import 'shell/app_router.dart';

/// 【模块 1】入口。只做三件事：装 JSB、起事件泵、进路由。
Future<void> main() async {
  WidgetsFlutterBinding.ensureInitialized();

  // [24] 初始化 C++ 核心（验签 → SQLite → 传感器）
  final dir = await getApplicationSupportDirectory();
  final rc = JsbClient.instance.init(dir.path, Platform.version.split(' ').first);
  if (rc != 0) {
    // 注意：验签失败也返回 0，UI 要起得来（只是置灰）。rc != 0 才是真故障。
    runApp(const _FatalApp());
    return;
  }

  // 事件泵：16ms 拉一次（不用 callback，避免跨 FFI 重入）
  JsbClient.instance.startPump();

  runApp(const YieyuApp());
}

class YieyuApp extends StatelessWidget {
  const YieyuApp({super.key});

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
    );
  }
}

class _FatalApp extends StatelessWidget {
  const _FatalApp();
  @override
  Widget build(BuildContext context) => const MaterialApp(
        home: Scaffold(
          body: Center(child: Text('核心初始化失败 —— 看 logcat 的 yieyu tag')),
        ),
      );
}
