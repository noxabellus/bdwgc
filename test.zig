const std = @import("std");
const gc = @import("libgc");

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