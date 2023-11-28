const std = @import("std");
var gpa = std.heap.GeneralPurposeAllocator(.{ .stack_trace_frames = 12 }){};
const alloc = gpa.allocator();

pub fn main() !void {
    defer _ = gpa.deinit();

    // our http client, this can make multiple requests (and is even threadsafe, although individual requests are not).
    var client = std.http.Client{
        .allocator = alloc,
    };

    // we can `catch unreachable` here because we can guarantee that this is a valid url.
    const uri = std.Uri.parse("https://example.com/") catch unreachable;

    // these are the headers we'll be sending to the server
    var headers = std.http.Headers{ .allocator = alloc };
    defer headers.deinit();

    try headers.append("accept", "*/*"); // tell the server we'll accept anything

    // make the connection and set up the request
    var req = try client.open(.GET, uri, headers, .{});
    defer req.deinit();

    // send the request and headers to the server.
    try req.send(.{});
    try req.wait();

    // read the content-type header from the server, or default to text/plain
    const content_type = req.response.headers.getFirstValue("content-type") orelse "text/plain";
    _ = content_type;

    // read the entire response body
    const body = req.reader().readAllAlloc(alloc, 1024 * 1024) catch unreachable;
    defer alloc.free(body);

    std.debug.print("{s}", .{body});
}
