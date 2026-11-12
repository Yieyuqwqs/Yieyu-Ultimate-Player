// 模块22 独立 Gradle 模块；由 Flutter 工程以本地模块方式 include。
// 全场唯一 Kotlin 代码就在这里，行数预算 400（见 docs/ARCH_24.md 铁律 7）。
pluginManagement {
    repositories { google(); mavenCentral(); gradlePluginPortal() }
}
dependencyResolutionManagement {
    repositories { google(); mavenCentral() }
}
rootProject.name = "yieyu-platform"
include(":platform-service")
