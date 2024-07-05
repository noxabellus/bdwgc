// source: https://github.com/mitchellh/zig-libgc

// MIT License

// Copyright (c) 2021 Mitchell Hashimoto

// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:

// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.

// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

const std = @import("std");
const assert = std.debug.assert;
const testing = std.testing;
const mem = std.mem;
const Allocator = std.mem.Allocator;

const gc = @cImport({
    @cInclude("gc.h");
});

/// Returns the Allocator used for APIs in Zig
pub fn allocator() Allocator {
    // Initialize libgc
    if (gc.GC_is_init_called() == 0) {
        gc.GC_init();
    }

    return Allocator{
        .ptr = undefined,
        .vtable = &gc_allocator_vtable,
    };
}

/// Enable or disable interior pointers.
/// If used, this must be called before the first allocator() call.
pub fn setAllInteriorPointers(enable_interior_pointers: bool) void {
    gc.GC_set_all_interior_pointers(@intFromBool(enable_interior_pointers));
}

/// Returns the current heap size of used memory.
pub fn getHeapSize() u64 {
    return gc.GC_get_heap_size();
}

/// Disable garbage collection.
pub fn disable() void {
    gc.GC_disable();
}

/// Enables garbage collection. GC is enabled by default so this is
/// only useful if you called disable earlier.
pub fn enable() void {
    gc.GC_enable();
}

// Performs a full, stop-the-world garbage collection. With leak detection
// enabled this will output any leaks as well.
pub fn collect() void {
    gc.GC_gcollect();
}

/// Perform some garbage collection. Returns zero when work is done.
pub fn collectLittle() u8 {
    return @as(u8, @intCast(gc.GC_collect_a_little()));
}

/// Enables leak-finding mode. See the libgc docs for more details.
pub fn setFindLeak(v: bool) void {
    return gc.GC_set_find_leak(@intFromBool(v));
}

// TODO there are so many more functions to add here
// from gc.h, just add em as they're useful.

/// GcAllocator is an implementation of std.mem.Allocator that uses
/// libgc under the covers. This means that all memory allocated with
/// this allocated doesn't need to be explicitly freed (but can be).
///
/// The GC is a singleton that is globally shared. Multiple GcAllocators
/// do not allocate separate pages of memory; they share the same underlying
/// pages.
///
// NOTE: this is basically just a copy of the standard CAllocator
// since libgc has a malloc/free-style interface. There are very slight differences
// due to API differences but overall the same.
pub const GcAllocator = struct {
    fn alloc(
        _: *anyopaque,
        len: usize,
        log2_align: u8,
        return_address: usize,
    ) ?[*]u8 {
        _ = return_address;
        assert(len > 0);
        return alignedAlloc(len, log2_align);
    }

    fn resize(
        _: *anyopaque,
        buf: []u8,
        log2_buf_align: u8,
        new_len: usize,
        return_address: usize,
    ) bool {
        _ = log2_buf_align;
        _ = return_address;
        if (new_len <= buf.len) {
            return true;
        }

        const full_len = alignedAllocSize(buf.ptr);
        if (new_len <= full_len) {
            return true;
        }

        return false;
    }

    fn free(
        _: *anyopaque,
        buf: []u8,
        log2_buf_align: u8,
        return_address: usize,
    ) void {
        _ = log2_buf_align;
        _ = return_address;
        alignedFree(buf.ptr);
    }

    fn getHeader(ptr: [*]u8) *[*]u8 {
        return @as(*[*]u8, @ptrFromInt(@intFromPtr(ptr) - @sizeOf(usize)));
    }

    fn alignedAlloc(len: usize, log2_align: u8) ?[*]u8 {
        const alignment = @as(usize, 1) << @as(Allocator.Log2Align, @intCast(log2_align));

        // Thin wrapper around regular malloc, overallocate to account for
        // alignment padding and store the orignal malloc()'ed pointer before
        // the aligned address.
        const unaligned_ptr = @as([*]u8, @ptrCast(gc.GC_malloc(len + alignment - 1 + @sizeOf(usize)) orelse return null));
        const unaligned_addr = @intFromPtr(unaligned_ptr);
        const aligned_addr = mem.alignForward(usize, unaligned_addr + @sizeOf(usize), alignment);
        const aligned_ptr = unaligned_ptr + (aligned_addr - unaligned_addr);
        getHeader(aligned_ptr).* = unaligned_ptr;

        return aligned_ptr;
    }

    fn alignedFree(ptr: [*]u8) void {
        const unaligned_ptr = getHeader(ptr).*;
        gc.GC_free(unaligned_ptr);
    }

    fn alignedAllocSize(ptr: [*]u8) usize {
        const unaligned_ptr = getHeader(ptr).*;
        const delta = @intFromPtr(ptr) - @intFromPtr(unaligned_ptr);
        return gc.GC_size(unaligned_ptr) - delta;
    }
};

const gc_allocator_vtable = Allocator.VTable{
    .alloc = GcAllocator.alloc,
    .resize = GcAllocator.resize,
    .free = GcAllocator.free,
};

