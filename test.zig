const std = @import("std");
const gc = @import("bdwgc");

test "GcAllocator" {
    std.debug.print("GcAllocator start\n", .{});
    const alloc = gc.allocator();

    try std.heap.testAllocator(alloc);
    try std.heap.testAllocatorAligned(alloc);
    try std.heap.testAllocatorLargeAlignment(alloc);
    try std.heap.testAllocatorAlignedShrink(alloc);
    std.debug.print("GcAllocator end\n", .{});
}

test "heap size" {
    std.debug.print("heap size start\n", .{});
    // No garbage so should be 0
    try std.testing.expect(gc.collectLittle() == 0);

    // Force a collection should work
    gc.collect();

    try std.testing.expect(gc.getHeapSize() > 0);
    std.debug.print("heap size end\n", .{});
}

test "finalizer" {
    std.debug.print("finalizer start\n", .{});

    const Test = struct {
        var final: isize = 0;
        var allocated: isize = 0;

        fn finalizer(_: ?*anyopaque, _: ?*anyopaque) callconv(.C) void {
            final += 1;
        }

        fn setup() !void {
            const alloc = gc.allocator();

            for (0..100) |_| {
                const ptr = try alloc.alloc(u8, 1024);

                allocated += 1;

                gc.registerFinalizer(@ptrCast(ptr.ptr), finalizer, null, null, null);
            }
        }

        fn approxEq(x: isize, y: isize) bool {
            return @abs(x - y) <= 2;
        }

        fn finish() !void {
            gc.collect();

            std.debug.print("ran {} / {} finalizers\n", .{final, allocated});
            try std.testing.expect(approxEq(allocated, final));

            std.debug.print("finalizer end\n", .{});
        }
    };

    try Test.setup();
    try Test.finish();
}