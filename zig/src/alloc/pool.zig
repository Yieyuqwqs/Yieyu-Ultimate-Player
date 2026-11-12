//! 【模块 4】定长内存池：抗碎片、无锁、可复用。
//!
//! 为什么不是 malloc：音频/解码链上的对象（PCM 块、FFmpeg packet、FlatBuffers buffer）
//! 生命周期短且尺寸固定，glibc/bionic 的 malloc 会碎、会锁、会 syscall。
//! 这里预先切好 slab，O(1) 拿放，永不向系统要内存（启动时一次拿满）。
const std = @import("std");

pub const Header = extern struct {
    magic: u32 = 0x594D454D, // 'YMEM'
    block_size: u32,
    block_count: u32,
    free_count: u32,
};

/// 多尺寸 slab：8B ~ 4KB，每档一块连续内存 + 自由链表
pub const Pool = struct {
    const sizes = [_]u32{ 16, 32, 64, 128, 256, 512, 1024, 2048, 4096 };

    backings: [sizes.len][]u8,
    free_lists: [sizes.len]?*Node,
    counts: [sizes.len]u32,

    const Node = struct { next: ?*Node };

    pub fn init(allocator: std.mem.Allocator, blocks_per_size: u32) !Pool {
        var p: Pool = undefined;
        for (sizes, 0..) |sz, i| {
            const total = @as(usize, sz) * blocks_per_size;
            p.backings[i] = try allocator.alloc(u8, total);
            p.counts[i] = blocks_per_size;
            p.free_lists[i] = null;
            // 切块并串成自由链表（初始化时一次性做完，运行期零成本）
            var off: usize = 0;
            while (off + sz <= total) : (off += sz) {
                const n: *Node = @ptrFromInt(@intFromPtr(p.backings[i].ptr) + off);
                n.next = p.free_lists[i];
                p.free_lists[i] = n;
            }
        }
        return p;
    }

    pub fn deinit(self: *Pool, allocator: std.mem.Allocator) void {
        for (self.backings) |b| allocator.free(b);
    }

    /// O(1) 分配；返回 null 表示该档耗尽（调用方应降级，而不是 panic）
    pub fn alloc(self: *Pool, size: usize) ?[]u8 {
        const idx = self.bucketOf(size) orelse return null;
        const node = self.free_lists[idx] orelse return null;
        self.free_lists[idx] = node.next;
        self.counts[idx] -= 1;
        const ptr: [*]u8 = @ptrCast(node);
        return ptr[0..sizes[idx]];
    }

    pub fn free(self: *Pool, slice: []u8) void {
        const idx = self.bucketOf(slice.len) orelse return;
        const node: *Node = @ptrCast(@alignCast(slice.ptr));
        node.next = self.free_lists[idx];
        self.free_lists[idx] = node;
        self.counts[idx] += 1;
    }

    fn bucketOf(_: *Pool, size: usize) ?usize {
        for (sizes, 0..) |sz, i| if (size <= sz) return i;
        return null; // >4KB 的块不归池管
    }
};

test "池子 O(1) 拿放，耗尽返回 null 而不是 panic" {
    const a = std.testing.allocator;
    var p = try Pool.init(a, 4);
    defer p.deinit(a);
    var got: [8][]u8 = undefined;
    var i: usize = 0;
    while (i < 8) : (i += 1) {
        got[i] = p.alloc(60) orelse break;
    }
    try std.testing.expect(i > 0);
    for (got[0..i]) |g| p.free(g);
}
