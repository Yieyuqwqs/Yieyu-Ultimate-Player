import 'package:flutter/material.dart';

import 'home_page.dart';

/// 【模块 1】宏观排版：路由表。**只干界面跳转**，不许出现任何业务判断。
class AppRouter {
  static const String home = '/';

  static Route<dynamic>? generate(RouteSettings settings) {
    switch (settings.name) {
      case home:
        return MaterialPageRoute(builder: (_) => const HomePage());
      default:
        return MaterialPageRoute(
          builder: (_) => Scaffold(body: Center(child: Text('未定义路由: ${settings.name}'))),
        );
    }
  }
}
