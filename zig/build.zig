//! 【模块 4】Zig CC / 分配器
//!
//! 只编"零依赖"的东西：内存池 + 定长分配器。**不碰 libc++ 边界**，
//! 因为 Android NDK 生态是 clang + libc++，Zig 混进来只做纯算法核不吃亏。
const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{ .preferred_optimize_mode = .ReleaseFast });

    // 产物：libyieyu_mem.a，由 core/CMakeLists.txt 静态链进 libyieyu_core.so
    const lib = b.addStaticLibrary(.{
        .name = "yieyu_mem",
        .root_source_file = b.path("src/alloc/pool.zig"),
        .target = target,
        .optimize = optimize,
    });
    lib.linkLibC();
    lib.bundle_compiler_rt = true; // 提供 __mulsi3 等符号，避免与 NDK 冲突
    b.installArtifact(lib);

    // 单测：zig build test
    const tests = b.addTest(.{
        .root_source_file = b.path("src/alloc/pool.zig"),
        .target = target,
        .optimize = optimize,
    });
    const run_tests = b.addRunArtifact(tests);
    b.step("test", "跑分配器单测").dependOn(&run_tests.step);
}
