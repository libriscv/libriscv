extern fn sys_write(dest: *const c_void, len: usize) callconv(.C) isize;
pub var args: [][*]u8 = undefined;

pub fn strlen(ptr: [*]u8) usize {
    var count: usize = 0;
    while (ptr[count] != 0) {
        count += 1;
    }
    return count;
}

inline fn write(dest: []const u8) isize
{
    return sys_write(&dest[0], dest.len);
}

export fn ZigMainCaller(c_argc: i32, c_argv: [*][*:0]u8) callconv(.C) i32 {
    args = c_argv[0..@intCast(usize, c_argc)];
    return main();
}

export fn main() i32 {
    _ = write("Hello World!\n");
    _ = write("Arg0: ");
    _ = sys_write(args[0], strlen(args[0]));
    _ = write("\n");
    return 0;
}
