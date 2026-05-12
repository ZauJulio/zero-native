const std = @import("std");
const runner = @import("runner");
const zero_native = @import("zero-native");

pub const panic = std.debug.FullPanic(zero_native.debug.capturePanic);

const app_permissions = [_][]const u8{zero_native.security.permission_window};
const bridge_origins = [_][]const u8{"zero://app"};
const window_permission = [_][]const u8{zero_native.security.permission_window};
const builtin_policies = [_]zero_native.BridgeCommandPolicy{
    .{ .name = "zero-native.webview.create", .permissions = &window_permission, .origins = &bridge_origins },
    .{ .name = "zero-native.webview.list", .permissions = &window_permission, .origins = &bridge_origins },
    .{ .name = "zero-native.webview.setFrame", .permissions = &window_permission, .origins = &bridge_origins },
    .{ .name = "zero-native.webview.navigate", .permissions = &window_permission, .origins = &bridge_origins },
    .{ .name = "zero-native.webview.setZoom", .permissions = &window_permission, .origins = &bridge_origins },
    .{ .name = "zero-native.webview.setLayer", .permissions = &window_permission, .origins = &bridge_origins },
    .{ .name = "zero-native.webview.close", .permissions = &window_permission, .origins = &bridge_origins },
};

const BrowserApp = struct {
    fn app(self: *@This()) zero_native.App {
        return .{
            .context = self,
            .name = "browser",
            .source = zero_native.frontend.productionSource(.{
                .dist = "frontend",
                .entry = "index.html",
                .origin = "zero://app",
                .spa_fallback = false,
            }),
        };
    }
};

pub fn main(init: std.process.Init) !void {
    var app = BrowserApp{};
    try runner.runWithOptions(app.app(), .{
        .app_name = "Zero Browser",
        .window_title = "Zero Browser",
        .bundle_id = "dev.zero_native.browser",
        .icon_path = "assets/icon.icns",
        .builtin_bridge = .{ .enabled = true, .commands = &builtin_policies },
        .security = .{
            .permissions = &app_permissions,
            .navigation = .{
                .allowed_origins = &.{"*"},
                .external_links = .{ .action = .deny },
            },
        },
    }, init);
}

test "browser app serves static frontend assets" {
    var state = BrowserApp{};
    const app = state.app();
    try std.testing.expectEqualStrings("browser", app.name);
    try std.testing.expectEqual(zero_native.WebViewSourceKind.assets, app.source.kind);
    try std.testing.expectEqualStrings("frontend", app.source.asset_options.?.root_path);
    try std.testing.expectEqualStrings("index.html", app.source.asset_options.?.entry);
}
