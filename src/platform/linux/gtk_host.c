#include "gtk_host.h"

#include <gtk/gtk.h>
#include <webkit/webkit.h>
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>

#define ZERO_NATIVE_MAX_WINDOWS 16
#define ZERO_NATIVE_MAX_WEBVIEWS 16

typedef struct zero_native_gtk_webview {
    char *label;
    WebKitWebView *web_view;
    double x;
    double y;
    double width;
    double height;
    int layer;
    int transparent;
    int bridge_enabled;
    WebKitUserContentManager *content_manager;
} zero_native_gtk_webview_t;

typedef struct zero_native_gtk_window {
    uint64_t id;
    GtkWindow *gtk_window;
    WebKitWebView *web_view;
    GtkWidget *stack_root;
    WebKitUserContentManager *content_manager;
    struct zero_native_gtk_host *host;
    char *label;
    char *title;
    char *asset_root;
    char *asset_entry;
    char *asset_origin;
    char *bridge_origin;
    int spa_fallback;
    double x;
    double y;
    zero_native_gtk_webview_t webviews[ZERO_NATIVE_MAX_WEBVIEWS];
    int webview_count;
} zero_native_gtk_window_t;

#define ZERO_NATIVE_MAX_NOTIFICATIONS 64

// Maps a live desktop-notification id (from org.freedesktop.Notifications) back
// to the WebKitNotification and the window that raised it, so a click on the
// desktop notification can focus the window and notify the page.
typedef struct zero_native_notification {
    guint32 dbus_id;
    WebKitNotification *notification; // owns a ref while pending
    uint64_t window_id;
} zero_native_notification_t;

struct zero_native_gtk_host {
    GtkApplication *app;
    char *app_name;
    char *window_title;
    char *bundle_id;
    char *icon_path;
    char *window_label;
    double init_x, init_y, init_width, init_height;
    int restore_frame;
    int decorated;

    zero_native_gtk_event_callback_t callback;
    void *callback_context;
    zero_native_gtk_bridge_callback_t bridge_callback;
    void *bridge_context;

    zero_native_gtk_window_t windows[ZERO_NATIVE_MAX_WINDOWS];
    int window_count;
    int did_shutdown;
    guint frame_timer;

    char **allowed_origins;
    int allowed_origins_count;
    char **allowed_external_urls;
    int allowed_external_urls_count;
    int external_link_action;
    int scheme_registered;

    // Persistent WebKit storage shared by every web view (cookies, IndexedDB,
    // local storage, service workers) so the WhatsApp session survives restarts.
    WebKitNetworkSession *session;
    char *data_dir;
    char *cache_dir;
    char *user_agent;

    // Desktop notifications forwarded to org.freedesktop.Notifications.
    GDBusConnection *bus;
    guint notif_action_sub;
    guint notif_closed_sub;
    zero_native_notification_t notifications[ZERO_NATIVE_MAX_NOTIFICATIONS];
    int notification_count;

    // System tray (StatusNotifierItem + com.canonical.dbusmenu).
    int tray_active;
    guint tray_sni_reg_id;
    guint tray_menu_reg_id;
    guint tray_watcher_watch;
    char *tray_tooltip;
    int tray_attention;
    int muted; // suppress notification sound when set
};

static char *zero_native_strndup(const char *s, size_t len) {
    char *out = malloc(len + 1);
    if (!out) return NULL;
    memcpy(out, s, len);
    out[len] = '\0';
    return out;
}

static void zero_native_free_string_list(char **list, int count) {
    if (!list) return;
    for (int i = 0; i < count; i++) free(list[i]);
    free(list);
}

static char **zero_native_parse_newline_list(const char *bytes, size_t len, int *out_count) {
    *out_count = 0;
    if (!bytes || len == 0) return NULL;

    int capacity = 4;
    char **result = malloc(sizeof(char *) * (size_t)capacity);
    if (!result) return NULL;

    const char *start = bytes;
    const char *end = bytes + len;
    while (start < end) {
        const char *nl = memchr(start, '\n', (size_t)(end - start));
        size_t seg_len = nl ? (size_t)(nl - start) : (size_t)(end - start);
        while (seg_len > 0 && (start[0] == ' ' || start[0] == '\t')) { start++; seg_len--; }
        while (seg_len > 0 && (start[seg_len - 1] == ' ' || start[seg_len - 1] == '\t' || start[seg_len - 1] == '\r')) seg_len--;
        if (seg_len > 0) {
            if (*out_count >= capacity) {
                capacity *= 2;
                char **tmp = realloc(result, sizeof(char *) * (size_t)capacity);
                if (!tmp) { zero_native_free_string_list(result, *out_count); *out_count = 0; return NULL; }
                result = tmp;
            }
            result[*out_count] = zero_native_strndup(start, seg_len);
            (*out_count)++;
        }
        start = nl ? nl + 1 : end;
    }
    return result;
}

static void zero_native_replace_string(char **dest, const char *bytes, size_t len) {
    free(*dest);
    *dest = bytes && len > 0 ? zero_native_strndup(bytes, len) : NULL;
}

static void zero_native_clear_window_source(zero_native_gtk_window_t *win) {
    free(win->asset_root);
    free(win->asset_entry);
    free(win->asset_origin);
    free(win->bridge_origin);
    win->asset_root = NULL;
    win->asset_entry = NULL;
    win->asset_origin = NULL;
    win->bridge_origin = NULL;
    win->spa_fallback = 0;
}

static int zero_native_valid_webview_frame(double x, double y, double width, double height) {
    return x >= 0 && y >= 0 && width > 0 && height > 0;
}

static int zero_native_webview_extent(double value) {
    return value > 1 ? (int)(value + 0.5) : 1;
}

static int zero_native_webview_coord(double value) {
    return value > 0 ? (int)(value + 0.5) : 0;
}

static void zero_native_apply_webview_frame(zero_native_gtk_webview_t *webview) {
    if (!webview || !webview->web_view) return;
    GtkWidget *widget = GTK_WIDGET(webview->web_view);
    gtk_widget_set_halign(widget, GTK_ALIGN_START);
    gtk_widget_set_valign(widget, GTK_ALIGN_START);
    gtk_widget_set_margin_start(widget, zero_native_webview_coord(webview->x));
    gtk_widget_set_margin_top(widget, zero_native_webview_coord(webview->y));
    gtk_widget_set_size_request(widget, zero_native_webview_extent(webview->width), zero_native_webview_extent(webview->height));
}

static void zero_native_reorder_webviews(zero_native_gtk_window_t *win) {
    if (!win || !win->stack_root) return;
    int placed[ZERO_NATIVE_MAX_WEBVIEWS] = {0};
    GtkWidget *previous = NULL;
    for (int pass = 0; pass < win->webview_count; pass++) {
        int best = -1;
        for (int i = 0; i < win->webview_count; i++) {
            if (!win->webviews[i].web_view) continue;
            if (placed[i]) continue;
            if (best < 0 || win->webviews[i].layer < win->webviews[best].layer) best = i;
        }
        if (best < 0) break;
        gtk_widget_insert_after(GTK_WIDGET(win->webviews[best].web_view), GTK_WIDGET(win->stack_root), previous);
        placed[best] = 1;
        previous = GTK_WIDGET(win->webviews[best].web_view);
    }
}

static void zero_native_clear_webview(zero_native_gtk_window_t *win, zero_native_gtk_webview_t *webview) {
    if (!webview) return;
    if (webview->web_view && win && win->stack_root) {
        gtk_overlay_remove_overlay(GTK_OVERLAY(win->stack_root), GTK_WIDGET(webview->web_view));
    }
    free(webview->label);
    memset(webview, 0, sizeof(*webview));
}

static void zero_native_remove_webview_at(zero_native_gtk_window_t *win, int index) {
    if (!win || index < 0 || index >= win->webview_count) return;
    zero_native_clear_webview(win, &win->webviews[index]);
    for (int i = index; i + 1 < win->webview_count; i++) {
        win->webviews[i] = win->webviews[i + 1];
    }
    memset(&win->webviews[win->webview_count - 1], 0, sizeof(win->webviews[win->webview_count - 1]));
    win->webview_count--;
}

static void zero_native_clear_webviews(zero_native_gtk_window_t *win) {
    if (!win) return;
    while (win->webview_count > 0) {
        zero_native_remove_webview_at(win, win->webview_count - 1);
    }
}

static void zero_native_clear_window(zero_native_gtk_window_t *win) {
    if (!win) return;
    zero_native_clear_webviews(win);
    zero_native_clear_window_source(win);
    free(win->label);
    free(win->title);
    memset(win, 0, sizeof(*win));
}

static char *zero_native_origin_for_uri(const char *uri) {
    if (!uri || !uri[0]) return g_strdup("zero://inline");
    const char *scheme_end = strstr(uri, "://");
    if (!scheme_end) return g_strdup("zero://inline");
    size_t scheme_len = (size_t)(scheme_end - uri);
    if (scheme_len == 5 && strncmp(uri, "about", 5) == 0) {
        return g_strdup("zero://inline");
    }
    if (scheme_len == 4 && strncmp(uri, "file", 4) == 0) {
        return g_strdup("file://local");
    }
    const char *host_start = scheme_end + 3;
    const char *host_end = host_start;
    while (*host_end && *host_end != '/' && *host_end != '?' && *host_end != '#') host_end++;
    if (host_end == host_start) {
        return g_strdup_printf("%.*s://local", (int)scheme_len, uri);
    }
    return g_strndup(uri, (gsize)(host_end - uri));
}

static int zero_native_policy_list_matches(char **values, int count, const char *uri) {
    char *origin = zero_native_origin_for_uri(uri);
    int matched = 0;
    for (int i = 0; i < count && !matched; i++) {
        const char *value = values[i];
        size_t len = strlen(value);
        if (strcmp(value, "*") == 0 || strcmp(value, origin) == 0 || (uri && strcmp(value, uri) == 0)) {
            matched = 1;
        } else if (len > 0 && value[len - 1] == '*') {
            matched = uri && strncmp(uri, value, len - 1) == 0;
            if (!matched) matched = strncmp(origin, value, len - 1) == 0;
        }
    }
    g_free(origin);
    return matched;
}

static int zero_native_path_is_safe(const char *path) {
    if (!path || !path[0]) return 0;
    if (path[0] == '/' || strchr(path, '\\')) return 0;
    const char *segment = path;
    while (*segment) {
        const char *slash = strchr(segment, '/');
        size_t len = slash ? (size_t)(slash - segment) : strlen(segment);
        if (len == 0) return 0;
        if ((len == 1 && segment[0] == '.') || (len == 2 && segment[0] == '.' && segment[1] == '.')) return 0;
        if (!slash) break;
        segment = slash + 1;
    }
    return 1;
}

static const char *zero_native_bridge_script(void) {
    return
        "(function(){"
        "if(window.zero&&window.zero.invoke){return;}"
        "var pending=new Map();"
        "var listeners=new Map();"
        "var nextId=1;"
        "function post(message){"
        "window.webkit.messageHandlers.zeroNativeBridge.postMessage(message);"
        "}"
        "function complete(response){"
        "var id=response&&response.id!=null?String(response.id):'';"
        "var entry=pending.get(id);"
        "if(!entry){return;}"
        "pending.delete(id);"
        "if(response.ok){entry.resolve(response.result===undefined?null:response.result);return;}"
        "var errorInfo=response.error||{};"
        "var error=new Error(errorInfo.message||'Native command failed');"
        "error.code=errorInfo.code||'internal_error';"
        "entry.reject(error);"
        "}"
        "function invoke(command,payload){"
        "if(typeof command!=='string'||command.length===0){return Promise.reject(new TypeError('command must be a non-empty string'));}"
        "var id=String(nextId++);"
        "var envelope=JSON.stringify({id:id,command:command,payload:payload===undefined?null:payload});"
        "return new Promise(function(resolve,reject){"
        "pending.set(id,{resolve:resolve,reject:reject});"
        "try{post(envelope);}catch(error){pending.delete(id);reject(error);}"
        "});"
        "}"
        "function selector(value){return typeof value==='number'?{id:value}:{label:String(value)};}"
        "function ensureString(value,name){if(typeof value!=='string'||value.length===0){throw new TypeError(name+' must be a non-empty string');}return value;}"
        "function ensureNumber(value,name){if(typeof value!=='number'||!isFinite(value)){throw new TypeError(name+' must be a finite number');}return value;}"
        "function validateWebViewSelector(options){if(options.label!=null){ensureString(options.label,'label');}if(options.windowId!=null&&(typeof options.windowId!=='number'||!isFinite(options.windowId)||options.windowId<0||Math.floor(options.windowId)!==options.windowId)){throw new TypeError('windowId must be a non-negative integer');}}"
        "function framePayload(options){options=options||{};validateWebViewSelector(options);var frame=options.frame||options;return {label:options.label,windowId:options.windowId,url:options.url,frame:{x:frame.x==null?0:ensureNumber(frame.x,'frame.x'),y:frame.y==null?0:ensureNumber(frame.y,'frame.y'),width:ensureNumber(frame.width,'frame.width'),height:ensureNumber(frame.height,'frame.height')}};}"
        "function createPayload(options){options=options||{};ensureString(options.url,'url');var payload=framePayload(options);if(options.layer!=null){payload.layer=ensureNumber(options.layer,'layer');}if(options.transparent!=null){payload.transparent=!!options.transparent;}if(options.bridge!=null){payload.bridge=!!options.bridge;}return payload;}"
        "function navigatePayload(options){options=options||{};validateWebViewSelector(options);ensureString(options.url,'url');return {label:options.label,windowId:options.windowId,url:options.url};}"
        "function closePayload(options){options=options||{};validateWebViewSelector(options);return {label:options.label,windowId:options.windowId};}"
        "function webviewHandle(info){return Object.freeze(Object.assign({},info,{setFrame:function(frame){return webviews.setFrame({label:info.label,windowId:info.windowId,frame:frame});},navigate:function(url){return webviews.navigate({label:info.label,windowId:info.windowId,url:url});},setZoom:function(zoom){return webviews.setZoom({label:info.label,windowId:info.windowId,zoom:zoom});},setLayer:function(layer){return webviews.setLayer({label:info.label,windowId:info.windowId,layer:layer});},close:function(){return webviews.close({label:info.label,windowId:info.windowId});}}));}"
        "function on(name,callback){if(typeof callback!=='function'){throw new TypeError('callback must be a function');}var set=listeners.get(name);if(!set){set=new Set();listeners.set(name,set);}set.add(callback);return function(){off(name,callback);};}"
        "function off(name,callback){var set=listeners.get(name);if(set){set.delete(callback);if(set.size===0){listeners.delete(name);}}}"
        "function emit(name,detail){var set=listeners.get(name);if(set){Array.from(set).forEach(function(callback){callback(detail);});}window.dispatchEvent(new CustomEvent('zero-native:'+name,{detail:detail}));}"
        "var windows=Object.freeze({"
        "create:function(options){return invoke('zero-native.window.create',options||{});},"
        "list:function(){return invoke('zero-native.window.list',{});},"
        "focus:function(value){return invoke('zero-native.window.focus',selector(value));},"
        "close:function(value){return invoke('zero-native.window.close',value==null?{}:selector(value));},"
        "minimize:function(value){return invoke('zero-native.window.minimize',value==null?{}:selector(value));},"
        "toggleMaximize:function(value){return invoke('zero-native.window.toggleMaximize',value==null?{}:selector(value));},"
        "setDecorated:function(decorated,value){var p=value==null?{}:selector(value);p.decorated=!!decorated;return invoke('zero-native.window.setDecorated',p);},"
        "startDrag:function(value){return invoke('zero-native.window.startDrag',value==null?{}:selector(value));},"
        "startResize:function(edge,value){var p=value==null?{}:selector(value);p.edge=String(edge);return invoke('zero-native.window.startResize',p);}"
        "});"
        "var dialogs=Object.freeze({"
        "openFile:function(options){return invoke('zero-native.dialog.openFile',options||{});},"
        "saveFile:function(options){return invoke('zero-native.dialog.saveFile',options||{});},"
        "showMessage:function(options){return invoke('zero-native.dialog.showMessage',options||{});}"
        "});"
        "function zoomPayload(options){options=options||{};validateWebViewSelector(options);return {label:options.label,windowId:options.windowId,zoom:ensureNumber(options.zoom,'zoom')};}"
        "function layerPayload(options){options=options||{};validateWebViewSelector(options);return {label:options.label,windowId:options.windowId,layer:ensureNumber(options.layer,'layer')};}"
        "var webviews=Object.freeze({"
        "create:function(options){return invoke('zero-native.webview.create',createPayload(options)).then(webviewHandle);},"
        "list:function(){return invoke('zero-native.webview.list',{});},"
        "setFrame:function(options){return invoke('zero-native.webview.setFrame',framePayload(options));},"
        "navigate:function(options){return invoke('zero-native.webview.navigate',navigatePayload(options));},"
        "setZoom:function(options){return invoke('zero-native.webview.setZoom',zoomPayload(options));},"
        "setLayer:function(options){return invoke('zero-native.webview.setLayer',layerPayload(options));},"
        "close:function(options){return invoke('zero-native.webview.close',closePayload(options));}"
        "});"
        "var tray=Object.freeze({"
        "update:function(options){return invoke('zero-native.tray.update',options||{});}"
        "});"
        "Object.defineProperty(window,'zero',{value:Object.freeze({invoke:invoke,on:on,off:off,windows:windows,dialogs:dialogs,webviews:webviews,tray:tray,_complete:complete,_emit:emit}),configurable:false});"
        "})();";
}

static const char *zero_native_mime_for_ext(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";
    dot++;
    if (strcmp(dot, "html") == 0 || strcmp(dot, "htm") == 0) return "text/html";
    if (strcmp(dot, "js") == 0 || strcmp(dot, "mjs") == 0) return "text/javascript";
    if (strcmp(dot, "css") == 0) return "text/css";
    if (strcmp(dot, "json") == 0) return "application/json";
    if (strcmp(dot, "svg") == 0) return "image/svg+xml";
    if (strcmp(dot, "png") == 0) return "image/png";
    if (strcmp(dot, "jpg") == 0 || strcmp(dot, "jpeg") == 0) return "image/jpeg";
    if (strcmp(dot, "gif") == 0) return "image/gif";
    if (strcmp(dot, "webp") == 0) return "image/webp";
    if (strcmp(dot, "woff") == 0) return "font/woff";
    if (strcmp(dot, "woff2") == 0) return "font/woff2";
    if (strcmp(dot, "ttf") == 0) return "font/ttf";
    if (strcmp(dot, "otf") == 0) return "font/otf";
    if (strcmp(dot, "wasm") == 0) return "application/wasm";
    return "application/octet-stream";
}

static zero_native_gtk_window_t *zero_native_window_for_asset_uri(zero_native_gtk_host_t *host, const char *uri) {
    char *origin = zero_native_origin_for_uri(uri);
    zero_native_gtk_window_t *fallback = NULL;
    for (int i = 0; i < host->window_count; i++) {
        zero_native_gtk_window_t *win = &host->windows[i];
        if (!win->asset_root) continue;
        if (!fallback) fallback = win;
        if (win->asset_origin && strcmp(win->asset_origin, origin) == 0) {
            g_free(origin);
            return win;
        }
    }
    g_free(origin);
    return fallback;
}

static char *zero_native_asset_relative_path(const char *uri, const char *entry) {
    const char *path = strstr(uri, "://");
    path = path ? strchr(path + 3, '/') : NULL;
    if (!path || !path[1]) return g_strdup(entry && entry[0] ? entry : "index.html");
    while (*path == '/') path++;
    char *unescaped = g_uri_unescape_string(path, NULL);
    if (!unescaped) return NULL;
    if (!zero_native_path_is_safe(unescaped)) {
        g_free(unescaped);
        return NULL;
    }
    return unescaped;
}

static void zero_native_fail_scheme_request(WebKitURISchemeRequest *request, GQuark domain, int code, const char *message) {
    GError *error = g_error_new_literal(domain, code, message);
    webkit_uri_scheme_request_finish_error(request, error);
    g_error_free(error);
}

static void zero_native_asset_scheme_request(WebKitURISchemeRequest *request, gpointer data) {
    zero_native_gtk_host_t *host = data;
    const char *uri = webkit_uri_scheme_request_get_uri(request);
    zero_native_gtk_window_t *win = zero_native_window_for_asset_uri(host, uri);
    if (!win || !win->asset_root) {
        zero_native_fail_scheme_request(request, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "No asset root is configured");
        return;
    }

    char *relative = zero_native_asset_relative_path(uri, win->asset_entry);
    if (!relative) {
        zero_native_fail_scheme_request(request, G_IO_ERROR, G_IO_ERROR_INVALID_FILENAME, "Unsafe asset path");
        return;
    }

    char *path = g_build_filename(win->asset_root, relative, NULL);
    if (!g_file_test(path, G_FILE_TEST_EXISTS) || g_file_test(path, G_FILE_TEST_IS_DIR)) {
        if (win->spa_fallback) {
            g_free(path);
            path = g_build_filename(win->asset_root, win->asset_entry && win->asset_entry[0] ? win->asset_entry : "index.html", NULL);
        }
    }

    gchar *contents = NULL;
    gsize length = 0;
    GError *read_error = NULL;
    if (!g_file_get_contents(path, &contents, &length, &read_error)) {
        webkit_uri_scheme_request_finish_error(request, read_error);
        g_error_free(read_error);
        g_free(path);
        g_free(relative);
        return;
    }

    GInputStream *stream = g_memory_input_stream_new_from_data(contents, (gssize)length, g_free);
    webkit_uri_scheme_request_finish(request, stream, (gint64)length, zero_native_mime_for_ext(path));
    g_object_unref(stream);
    g_free(path);
    g_free(relative);
}

static zero_native_gtk_window_t *zero_native_find_window(zero_native_gtk_host_t *host, uint64_t id) {
    for (int i = 0; i < host->window_count; i++) {
        if (host->windows[i].id == id && host->windows[i].gtk_window) return &host->windows[i];
    }
    return NULL;
}

static zero_native_gtk_webview_t *zero_native_find_webview(zero_native_gtk_window_t *win, const char *label) {
    if (!win || !label) return NULL;
    for (int i = 0; i < win->webview_count; i++) {
        if (win->webviews[i].label && strcmp(win->webviews[i].label, label) == 0) return &win->webviews[i];
    }
    return NULL;
}

static void zero_native_emit(zero_native_gtk_host_t *host, zero_native_gtk_event_t event) {
    if (host->callback) host->callback(host->callback_context, &event);
}

static void zero_native_emit_window_frame(zero_native_gtk_host_t *host, zero_native_gtk_window_t *win, int open) {
    if (!win || !win->gtk_window) return;
    int w = gtk_widget_get_width(GTK_WIDGET(win->gtk_window));
    int h = gtk_widget_get_height(GTK_WIDGET(win->gtk_window));
    GdkSurface *surface = gtk_native_get_surface(GTK_NATIVE(win->gtk_window));
    double scale = surface ? gdk_surface_get_scale_factor(surface) : 1.0;
    int focused = gtk_window_is_active(win->gtk_window) ? 1 : 0;
    zero_native_emit(host, (zero_native_gtk_event_t){
        .kind = ZERO_NATIVE_GTK_EVENT_WINDOW_FRAME,
        .window_id = win->id,
        .x = win->x, .y = win->y,
        .width = (double)w, .height = (double)h,
        .scale = scale,
        .open = open,
        .focused = focused,
        .label = win->label ? win->label : "",
        .label_len = win->label ? strlen(win->label) : 0,
        .title = win->title ? win->title : "",
        .title_len = win->title ? strlen(win->title) : 0,
    });
}

static void zero_native_emit_resize(zero_native_gtk_host_t *host, zero_native_gtk_window_t *win) {
    if (!win || !win->gtk_window) return;
    int w = gtk_widget_get_width(GTK_WIDGET(win->gtk_window));
    int h = gtk_widget_get_height(GTK_WIDGET(win->gtk_window));
    GdkSurface *surface = gtk_native_get_surface(GTK_NATIVE(win->gtk_window));
    double scale = surface ? gdk_surface_get_scale_factor(surface) : 1.0;
    zero_native_emit(host, (zero_native_gtk_event_t){
        .kind = ZERO_NATIVE_GTK_EVENT_RESIZE,
        .window_id = win->id,
        .width = (double)w, .height = (double)h,
        .scale = scale,
    });
}

static gboolean zero_native_frame_tick(gpointer data) {
    zero_native_gtk_host_t *host = data;
    zero_native_emit(host, (zero_native_gtk_event_t){ .kind = ZERO_NATIVE_GTK_EVENT_FRAME });
    return G_SOURCE_CONTINUE;
}

static void on_resize(GtkWidget *widget, GParamSpec *pspec, gpointer data) {
    (void)pspec;
    (void)widget;
    zero_native_gtk_window_t *win = data;
    zero_native_emit_window_frame(win->host, win, 1);
    zero_native_emit_resize(win->host, win);
}

static void on_focus(GtkWindow *window, GParamSpec *pspec, gpointer data) {
    (void)pspec;
    (void)window;
    zero_native_gtk_window_t *win = data;
    zero_native_emit_window_frame(win->host, win, 1);
}

static gboolean on_close_request(GtkWindow *window, gpointer data) {
    zero_native_gtk_window_t *win = data;
    zero_native_gtk_host_t *host = win->host;
    // Minimize the primary window to the tray instead of quitting.
    if (host->tray_active && win->id == 1) {
        gtk_widget_set_visible(GTK_WIDGET(window), FALSE);
        return TRUE; // stop the default close
    }
    int closed_index = -1;
    for (int i = 0; i < host->window_count; i++) {
        if (&host->windows[i] == win) {
            closed_index = i;
            break;
        }
    }
    zero_native_emit_window_frame(host, win, 0);

    if (closed_index >= 0) {
        zero_native_clear_window(&host->windows[closed_index]);
    }

    int open_count = 0;
    for (int i = 0; i < host->window_count; i++) {
        if (host->windows[i].gtk_window) open_count++;
    }
    if (open_count == 0) {
        if (!host->did_shutdown) {
            host->did_shutdown = 1;
            zero_native_emit(host, (zero_native_gtk_event_t){ .kind = ZERO_NATIVE_GTK_EVENT_SHUTDOWN });
        }
        if (host->frame_timer) {
            g_source_remove(host->frame_timer);
            host->frame_timer = 0;
        }
        g_application_quit(G_APPLICATION(host->app));
    }
    return FALSE;
}

static const char *zero_native_decision_uri(WebKitPolicyDecision *decision, WebKitPolicyDecisionType type) {
    if (type != WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION) return NULL;
    WebKitNavigationPolicyDecision *navigation = WEBKIT_NAVIGATION_POLICY_DECISION(decision);
    WebKitNavigationAction *action = webkit_navigation_policy_decision_get_navigation_action(navigation);
    WebKitURIRequest *request = action ? webkit_navigation_action_get_request(action) : NULL;
    return request ? webkit_uri_request_get_uri(request) : NULL;
}

#if GTK_CHECK_VERSION(4, 10, 0)
static void zero_native_uri_launch_done(GObject *source_object, GAsyncResult *result, gpointer data) {
    (void)data;
    GtkUriLauncher *launcher = GTK_URI_LAUNCHER(source_object);
    GError *error = NULL;
    if (!gtk_uri_launcher_launch_finish(launcher, result, &error) && error) {
        g_warning("failed to open external URI: %s", error->message);
        g_error_free(error);
    }
    g_object_unref(launcher);
}
#endif

static void zero_native_open_external_uri(GtkWindow *parent, const char *uri) {
#if GTK_CHECK_VERSION(4, 10, 0)
    GtkUriLauncher *launcher = gtk_uri_launcher_new(uri);
    gtk_uri_launcher_launch(launcher, parent, NULL, zero_native_uri_launch_done, NULL);
#else
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    gtk_show_uri(parent, uri, GDK_CURRENT_TIME);
    G_GNUC_END_IGNORE_DEPRECATIONS
#endif
}

static gboolean on_decide_policy(WebKitWebView *web_view, WebKitPolicyDecision *decision, WebKitPolicyDecisionType type, gpointer data) {
    (void)web_view;
    zero_native_gtk_window_t *win = data;
    zero_native_gtk_host_t *host = win->host;
    const char *uri = zero_native_decision_uri(decision, type);
    if (!uri || !uri[0] || strncmp(uri, "about:", 6) == 0) {
        webkit_policy_decision_use(decision);
        return TRUE;
    }

    char *origin = zero_native_origin_for_uri(uri);
    int internal_asset = win->asset_origin && strcmp(origin, win->asset_origin) == 0;
    g_free(origin);
    if (internal_asset || zero_native_policy_list_matches(host->allowed_origins, host->allowed_origins_count, uri)) {
        webkit_policy_decision_use(decision);
        return TRUE;
    }

    if (host->external_link_action == 1 && zero_native_policy_list_matches(host->allowed_external_urls, host->allowed_external_urls_count, uri)) {
        zero_native_open_external_uri(win->gtk_window, uri);
        webkit_policy_decision_ignore(decision);
        return TRUE;
    }

    webkit_policy_decision_ignore(decision);
    return TRUE;
}

static gboolean on_webview_decide_policy(WebKitWebView *web_view, WebKitPolicyDecision *decision, WebKitPolicyDecisionType type, gpointer data) {
    (void)web_view;
    zero_native_gtk_window_t *win = data;
    zero_native_gtk_host_t *host = win->host;
    const char *uri = zero_native_decision_uri(decision, type);
    if (!uri || !uri[0] || strncmp(uri, "about:", 6) == 0) {
        webkit_policy_decision_use(decision);
        return TRUE;
    }
    char *origin = zero_native_origin_for_uri(uri);
    int internal_asset = win->asset_origin && strcmp(origin, win->asset_origin) == 0;
    g_free(origin);
    if (internal_asset || zero_native_policy_list_matches(host->allowed_origins, host->allowed_origins_count, uri)) {
        webkit_policy_decision_use(decision);
        return TRUE;
    }
    if (host->external_link_action == 1 && zero_native_policy_list_matches(host->allowed_external_urls, host->allowed_external_urls_count, uri)) {
        zero_native_open_external_uri(win->gtk_window, uri);
    }
    webkit_policy_decision_ignore(decision);
    return TRUE;
}

static void on_bridge_message(WebKitUserContentManager *manager, JSCValue *js_result, gpointer data) {
    zero_native_gtk_window_t *win = data;
    zero_native_gtk_host_t *host = win->host;
    if (!host->bridge_callback) return;

    char *message = jsc_value_to_string(js_result);
    if (!message) return;

    const char *label = "main";
    WebKitWebView *source_webview = win->web_view;
    if (manager != win->content_manager) {
        for (int i = 0; i < win->webview_count; i++) {
            if (win->webviews[i].content_manager == manager) {
                label = win->webviews[i].label ? win->webviews[i].label : "webview";
                source_webview = win->webviews[i].web_view;
                break;
            }
        }
    }
    const char *uri = webkit_web_view_get_uri(source_webview);
    char *computed_origin = win->bridge_origin && strcmp(label, "main") == 0 ? g_strdup(win->bridge_origin) : zero_native_origin_for_uri(uri);
    host->bridge_callback(host->bridge_context, win->id, label, strlen(label), message, strlen(message), computed_origin, strlen(computed_origin));
    g_free(computed_origin);
    g_free(message);
}

static void zero_native_setup_bridge(zero_native_gtk_window_t *win) {
    WebKitUserContentManager *manager = win->content_manager;
    g_signal_connect(manager, "script-message-received::zeroNativeBridge", G_CALLBACK(on_bridge_message), win);
    webkit_user_content_manager_register_script_message_handler(manager, "zeroNativeBridge", NULL);

    WebKitUserScript *script = webkit_user_script_new(
        zero_native_bridge_script(),
        WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
        WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START,
        NULL, NULL);
    webkit_user_content_manager_add_script(manager, script);
    webkit_user_script_unref(script);
}

// ── Desktop notifications (org.freedesktop.Notifications over GDBus) ──

static zero_native_notification_t *zero_native_notification_find_by_id(zero_native_gtk_host_t *host, guint32 dbus_id) {
    for (int i = 0; i < host->notification_count; i++) {
        if (host->notifications[i].dbus_id == dbus_id) return &host->notifications[i];
    }
    return NULL;
}

static zero_native_notification_t *zero_native_notification_find_by_object(zero_native_gtk_host_t *host, WebKitNotification *notification) {
    for (int i = 0; i < host->notification_count; i++) {
        if (host->notifications[i].notification == notification) return &host->notifications[i];
    }
    return NULL;
}

static void zero_native_notification_remove(zero_native_gtk_host_t *host, zero_native_notification_t *entry) {
    if (!entry) return;
    if (entry->notification) g_object_unref(entry->notification);
    int index = (int)(entry - host->notifications);
    if (index < 0 || index >= host->notification_count) return;
    host->notifications[index] = host->notifications[host->notification_count - 1];
    host->notification_count--;
}

static void zero_native_present_window(zero_native_gtk_host_t *host, uint64_t window_id) {
    zero_native_gtk_window_t *win = zero_native_find_window(host, window_id);
    if (win && win->gtk_window) {
        gtk_window_unminimize(win->gtk_window);
        gtk_window_present(win->gtk_window);
    }
}

static void on_dbus_action_invoked(GDBusConnection *connection, const char *sender, const char *path, const char *iface, const char *signal_name, GVariant *params, gpointer data) {
    (void)connection; (void)sender; (void)path; (void)iface; (void)signal_name;
    zero_native_gtk_host_t *host = data;
    guint32 id = 0;
    const char *action = NULL;
    g_variant_get(params, "(u&s)", &id, &action);
    zero_native_notification_t *entry = zero_native_notification_find_by_id(host, id);
    if (!entry) return;
    // Let WhatsApp open the relevant chat, then raise our window.
    webkit_notification_clicked(entry->notification);
    zero_native_present_window(host, entry->window_id);
}

static void on_dbus_notification_closed(GDBusConnection *connection, const char *sender, const char *path, const char *iface, const char *signal_name, GVariant *params, gpointer data) {
    (void)connection; (void)sender; (void)path; (void)iface; (void)signal_name;
    zero_native_gtk_host_t *host = data;
    guint32 id = 0, reason = 0;
    g_variant_get(params, "(uu)", &id, &reason);
    zero_native_notification_remove(host, zero_native_notification_find_by_id(host, id));
}

// Lazily connect to the session bus and subscribe to notification signals.
static GDBusConnection *zero_native_host_bus(zero_native_gtk_host_t *host) {
    if (host->bus) return host->bus;
    host->bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
    if (!host->bus) return NULL;
    host->notif_action_sub = g_dbus_connection_signal_subscribe(
        host->bus, "org.freedesktop.Notifications", "org.freedesktop.Notifications", "ActionInvoked",
        "/org/freedesktop/Notifications", NULL, G_DBUS_SIGNAL_FLAGS_NONE, on_dbus_action_invoked, host, NULL);
    host->notif_closed_sub = g_dbus_connection_signal_subscribe(
        host->bus, "org.freedesktop.Notifications", "org.freedesktop.Notifications", "NotificationClosed",
        "/org/freedesktop/Notifications", NULL, G_DBUS_SIGNAL_FLAGS_NONE, on_dbus_notification_closed, host, NULL);
    return host->bus;
}

typedef struct {
    zero_native_gtk_host_t *host;
    WebKitNotification *notification; // owns a ref
    uint64_t window_id;
} zero_native_notify_ctx_t;

static void on_dbus_notify_sent(GObject *source, GAsyncResult *res, gpointer user_data) {
    zero_native_notify_ctx_t *ctx = user_data;
    GError *err = NULL;
    GVariant *ret = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &err);
    if (!ret) {
        if (err) g_error_free(err);
        if (ctx->notification) g_object_unref(ctx->notification);
        free(ctx);
        return;
    }
    guint32 id = 0;
    g_variant_get(ret, "(u)", &id);
    g_variant_unref(ret);

    zero_native_gtk_host_t *host = ctx->host;
    if (host->notification_count < ZERO_NATIVE_MAX_NOTIFICATIONS) {
        host->notifications[host->notification_count++] = (zero_native_notification_t){
            .dbus_id = id,
            .notification = ctx->notification, // transfer ownership
            .window_id = ctx->window_id,
        };
    } else {
        g_object_unref(ctx->notification);
    }
    free(ctx);
}

static void on_webkit_notification_closed(WebKitNotification *notification, gpointer data) {
    zero_native_gtk_host_t *host = data;
    zero_native_notification_t *entry = zero_native_notification_find_by_object(host, notification);
    if (!entry) return;
    GDBusConnection *bus = host->bus;
    if (bus) {
        g_dbus_connection_call(bus, "org.freedesktop.Notifications", "/org/freedesktop/Notifications",
            "org.freedesktop.Notifications", "CloseNotification",
            g_variant_new("(u)", entry->dbus_id), NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
    }
    zero_native_notification_remove(host, entry);
}

static gboolean on_show_notification(WebKitWebView *web_view, WebKitNotification *notification, gpointer data) {
    (void)web_view;
    zero_native_gtk_window_t *win = data;
    zero_native_gtk_host_t *host = win->host;
    GDBusConnection *bus = zero_native_host_bus(host);
    if (!bus) return FALSE; // let WebKit fall back to its default handling

    const char *title = webkit_notification_get_title(notification);
    const char *body = webkit_notification_get_body(notification);

    GVariantBuilder actions;
    g_variant_builder_init(&actions, G_VARIANT_TYPE("as"));
    g_variant_builder_add(&actions, "s", "default");
    g_variant_builder_add(&actions, "s", "Open");

    GVariantBuilder hints;
    g_variant_builder_init(&hints, G_VARIANT_TYPE("a{sv}"));
    if (host->bundle_id) g_variant_builder_add(&hints, "{sv}", "desktop-entry", g_variant_new_string(host->bundle_id));
    if (!host->muted) g_variant_builder_add(&hints, "{sv}", "sound-name", g_variant_new_string("message-new-instant"));

    zero_native_notify_ctx_t *ctx = malloc(sizeof(*ctx));
    ctx->host = host;
    ctx->window_id = win->id;
    ctx->notification = g_object_ref(notification);

    g_dbus_connection_call(bus, "org.freedesktop.Notifications", "/org/freedesktop/Notifications",
        "org.freedesktop.Notifications", "Notify",
        g_variant_new("(susssasa{sv}i)",
            host->app_name ? host->app_name : "zero-native", 0u,
            host->icon_path ? host->icon_path : "",
            title ? title : "", body ? body : "",
            &actions, &hints, -1),
        G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, -1, NULL, on_dbus_notify_sent, ctx);

    g_signal_connect(notification, "closed", G_CALLBACK(on_webkit_notification_closed), host);
    return TRUE; // we own display of this notification
}

static gboolean on_permission_request(WebKitWebView *web_view, WebKitPermissionRequest *request, gpointer data) {
    (void)web_view; (void)data;
    if (WEBKIT_IS_NOTIFICATION_PERMISSION_REQUEST(request) || WEBKIT_IS_USER_MEDIA_PERMISSION_REQUEST(request)) {
        webkit_permission_request_allow(request);
        return TRUE;
    }
    return FALSE; // defer everything else to the default handler
}

static void zero_native_append_json_string(GString *out, const char *str) {
    g_string_append_c(out, '"');
    for (const char *p = str ? str : ""; *p; p++) {
        switch (*p) {
            case '"': g_string_append(out, "\\\""); break;
            case '\\': g_string_append(out, "\\\\"); break;
            case '\n': g_string_append(out, "\\n"); break;
            case '\r': g_string_append(out, "\\r"); break;
            case '\t': g_string_append(out, "\\t"); break;
            default:
                if ((unsigned char)*p < 0x20) {
                    g_string_append_printf(out, "\\u%04x", (unsigned char)*p);
                } else {
                    g_string_append_c(out, *p);
                }
        }
    }
    g_string_append_c(out, '"');
}

// Relay a child web view's <title> changes to the chrome web view as a
// "webview:title" event so the app can mirror unread counts, etc.
static void on_child_title_changed(GObject *object, GParamSpec *pspec, gpointer data) {
    (void)pspec;
    WebKitWebView *wv = WEBKIT_WEB_VIEW(object);
    zero_native_gtk_window_t *win = data;
    const char *label = NULL;
    for (int i = 0; i < win->webview_count; i++) {
        if (win->webviews[i].web_view == wv) {
            label = win->webviews[i].label;
            break;
        }
    }
    if (!label) return; // only relay child web views, not the chrome itself
    const char *title = webkit_web_view_get_title(wv);
    GString *detail = g_string_new("{\"label\":");
    zero_native_append_json_string(detail, label);
    g_string_append(detail, ",\"title\":");
    zero_native_append_json_string(detail, title ? title : "");
    g_string_append_c(detail, '}');
    static const char event_name[] = "webview:title";
    zero_native_gtk_emit_window_event(win->host, win->id, event_name, sizeof(event_name) - 1, detail->str, detail->len);
    g_string_free(detail, TRUE);
}

static void zero_native_connect_webview_signals(zero_native_gtk_window_t *win, WebKitWebView *wv) {
    g_signal_connect(wv, "permission-request", G_CALLBACK(on_permission_request), win);
    g_signal_connect(wv, "show-notification", G_CALLBACK(on_show_notification), win);
    g_signal_connect(wv, "notify::title", G_CALLBACK(on_child_title_changed), win);
}

// ── System tray: StatusNotifierItem + com.canonical.dbusmenu over GDBus ──

#define ZERO_NATIVE_TRAY_ITEM_SHOW 1
#define ZERO_NATIVE_TRAY_ITEM_MUTE 2
#define ZERO_NATIVE_TRAY_ITEM_AUTOSTART 3
#define ZERO_NATIVE_TRAY_ITEM_LOCK 4
#define ZERO_NATIVE_TRAY_ITEM_QUIT 5

static const char zero_native_sni_xml[] =
    "<node><interface name='org.kde.StatusNotifierItem'>"
    "<property name='Category' type='s' access='read'/>"
    "<property name='Id' type='s' access='read'/>"
    "<property name='Title' type='s' access='read'/>"
    "<property name='Status' type='s' access='read'/>"
    "<property name='IconName' type='s' access='read'/>"
    "<property name='IconPixmap' type='a(iiay)' access='read'/>"
    "<property name='AttentionIconName' type='s' access='read'/>"
    "<property name='OverlayIconName' type='s' access='read'/>"
    "<property name='ToolTip' type='(sa(iiay)ss)' access='read'/>"
    "<property name='ItemIsMenu' type='b' access='read'/>"
    "<property name='Menu' type='o' access='read'/>"
    "<method name='Activate'><arg name='x' type='i' direction='in'/><arg name='y' type='i' direction='in'/></method>"
    "<method name='SecondaryActivate'><arg name='x' type='i' direction='in'/><arg name='y' type='i' direction='in'/></method>"
    "<method name='ContextMenu'><arg name='x' type='i' direction='in'/><arg name='y' type='i' direction='in'/></method>"
    "<method name='Scroll'><arg name='delta' type='i' direction='in'/><arg name='orientation' type='s' direction='in'/></method>"
    "<signal name='NewTitle'/><signal name='NewIcon'/><signal name='NewAttentionIcon'/>"
    "<signal name='NewOverlayIcon'/><signal name='NewToolTip'/>"
    "<signal name='NewStatus'><arg name='status' type='s'/></signal>"
    "</interface></node>";

static const char zero_native_dbusmenu_xml[] =
    "<node><interface name='com.canonical.dbusmenu'>"
    "<property name='Version' type='u' access='read'/>"
    "<property name='Status' type='s' access='read'/>"
    "<property name='TextDirection' type='s' access='read'/>"
    "<property name='IconThemePath' type='as' access='read'/>"
    "<method name='GetLayout'>"
    "<arg name='parentId' type='i' direction='in'/><arg name='recursionDepth' type='i' direction='in'/>"
    "<arg name='propertyNames' type='as' direction='in'/>"
    "<arg name='revision' type='u' direction='out'/><arg name='layout' type='(ia{sv}av)' direction='out'/></method>"
    "<method name='GetGroupProperties'>"
    "<arg name='ids' type='ai' direction='in'/><arg name='propertyNames' type='as' direction='in'/>"
    "<arg name='properties' type='a(ia{sv})' direction='out'/></method>"
    "<method name='GetProperty'>"
    "<arg name='id' type='i' direction='in'/><arg name='name' type='s' direction='in'/>"
    "<arg name='value' type='v' direction='out'/></method>"
    "<method name='Event'>"
    "<arg name='id' type='i' direction='in'/><arg name='eventId' type='s' direction='in'/>"
    "<arg name='data' type='v' direction='in'/><arg name='timestamp' type='u' direction='in'/></method>"
    "<method name='AboutToShow'><arg name='id' type='i' direction='in'/><arg name='needUpdate' type='b' direction='out'/></method>"
    "<signal name='LayoutUpdated'><arg name='revision' type='u'/><arg name='parent' type='i'/></signal>"
    "<signal name='ItemsPropertiesUpdated'><arg name='updatedProps' type='a(ia{sv})'/><arg name='removedProps' type='a(ias)'/></signal>"
    "</interface></node>";

static GDBusNodeInfo *zero_native_sni_node = NULL;
static GDBusNodeInfo *zero_native_dbusmenu_node = NULL;

// Build the IconPixmap variant a(iiay) (ARGB32, network order). This ALWAYS
// returns a valid, non-empty pixmap: if the icon file cannot be loaded it falls
// back to a solid square, because an empty/zero-size tray icon can wedge some
// status-notifier hosts (e.g. the GNOME AppIndicator extension) into flooding
// allocation assertions and hanging the shell.
static GVariant *zero_native_tray_icon_pixmap(const char *icon_path) {
    int w = 24, h = 24;
    guchar *argb = NULL;
    GdkPixbuf *pixbuf = (icon_path && icon_path[0])
        ? gdk_pixbuf_new_from_file_at_scale(icon_path, 24, 24, TRUE, NULL)
        : NULL;
    if (pixbuf) {
        w = gdk_pixbuf_get_width(pixbuf);
        h = gdk_pixbuf_get_height(pixbuf);
        if (w < 1 || h < 1) {
            g_object_unref(pixbuf);
            pixbuf = NULL;
        }
    }
    if (pixbuf) {
        int channels = gdk_pixbuf_get_n_channels(pixbuf);
        int stride = gdk_pixbuf_get_rowstride(pixbuf);
        const guchar *pixels = gdk_pixbuf_read_pixels(pixbuf);
        argb = g_malloc((gsize)w * h * 4);
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                const guchar *p = pixels + y * stride + x * channels;
                guchar a = channels == 4 ? p[3] : 0xff;
                guchar *o = argb + ((gsize)y * w + x) * 4;
                o[0] = a; o[1] = p[0]; o[2] = p[1]; o[3] = p[2]; // ARGB, network order
            }
        }
        g_object_unref(pixbuf);
    } else {
        // Fallback: solid WhatsApp-green square so the icon is never empty.
        w = 24;
        h = 24;
        argb = g_malloc((gsize)w * h * 4);
        for (gsize i = 0; i < (gsize)w * h; i++) {
            guchar *o = argb + i * 4;
            o[0] = 0xff; o[1] = 0x25; o[2] = 0xd3; o[3] = 0x66;
        }
    }
    GVariant *bytes = g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, argb, (gsize)w * h * 4, 1);
    GVariantBuilder builder;
    g_variant_builder_init(&builder, G_VARIANT_TYPE("a(iiay)"));
    g_variant_builder_add(&builder, "(ii@ay)", w, h, bytes);
    g_free(argb);
    return g_variant_builder_end(&builder);
}

// ── Autostart (XDG ~/.config/autostart/<bundle>.desktop) ──

static char *zero_native_autostart_path(zero_native_gtk_host_t *host) {
    char *name = g_strconcat(host->bundle_id ? host->bundle_id : "zero-native", ".desktop", NULL);
    char *path = g_build_filename(g_get_user_config_dir(), "autostart", name, NULL);
    g_free(name);
    return path;
}

static int zero_native_autostart_enabled(zero_native_gtk_host_t *host) {
    char *path = zero_native_autostart_path(host);
    int exists = g_file_test(path, G_FILE_TEST_EXISTS);
    g_free(path);
    return exists;
}

static void zero_native_set_autostart(zero_native_gtk_host_t *host, int enable) {
    char *path = zero_native_autostart_path(host);
    if (enable) {
        char *dir = g_path_get_dirname(path);
        g_mkdir_with_parents(dir, 0700);
        g_free(dir);
        char exe[4096];
        ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
        if (n > 0) {
            exe[n] = '\0';
        } else {
            g_strlcpy(exe, host->app_name ? host->app_name : "zero-native", sizeof(exe));
        }
        char *contents = g_strdup_printf(
            "[Desktop Entry]\nType=Application\nName=%s\nExec=%s\nIcon=%s\nX-GNOME-Autostart-enabled=true\nTerminal=false\n",
            host->app_name ? host->app_name : "ZeroWhats",
            exe,
            host->bundle_id ? host->bundle_id : "zero-native");
        g_file_set_contents(path, contents, -1, NULL);
        g_free(contents);
    } else {
        g_remove(path);
    }
    g_free(path);
}

static void zero_native_tray_toggle_window(zero_native_gtk_host_t *host) {
    zero_native_gtk_window_t *win = zero_native_find_window(host, 1);
    if (!win || !win->gtk_window) return;
    if (gtk_widget_get_visible(GTK_WIDGET(win->gtk_window)) && gtk_window_is_active(win->gtk_window)) {
        gtk_widget_set_visible(GTK_WIDGET(win->gtk_window), FALSE);
    } else {
        gtk_widget_set_visible(GTK_WIDGET(win->gtk_window), TRUE);
        gtk_window_present(win->gtk_window);
    }
}

static void zero_native_tray_menu_changed(zero_native_gtk_host_t *host) {
    if (!host->bus) return;
    g_dbus_connection_emit_signal(host->bus, NULL, "/MenuBar", "com.canonical.dbusmenu",
        "LayoutUpdated", g_variant_new("(ui)", 2u, 0), NULL);
}

static void zero_native_tray_invoke(zero_native_gtk_host_t *host, guint32 item_id) {
    switch (item_id) {
        case ZERO_NATIVE_TRAY_ITEM_QUIT:
            g_application_quit(G_APPLICATION(host->app));
            break;
        case ZERO_NATIVE_TRAY_ITEM_MUTE:
            host->muted = !host->muted;
            zero_native_tray_menu_changed(host);
            break;
        case ZERO_NATIVE_TRAY_ITEM_AUTOSTART:
            zero_native_set_autostart(host, !zero_native_autostart_enabled(host));
            zero_native_tray_menu_changed(host);
            break;
        case ZERO_NATIVE_TRAY_ITEM_LOCK: {
            static const char ev[] = "app:lock";
            zero_native_gtk_emit_window_event(host, 1, ev, sizeof(ev) - 1, "{}", 2);
            break;
        }
        case ZERO_NATIVE_TRAY_ITEM_SHOW:
        default:
            zero_native_tray_toggle_window(host);
            break;
    }
}

static GVariant *zero_native_menu_item_node(int id, const char *label, int checkable, int checked, int separator) {
    GVariantBuilder props;
    g_variant_builder_init(&props, G_VARIANT_TYPE("a{sv}"));
    if (separator) {
        g_variant_builder_add(&props, "{sv}", "type", g_variant_new_string("separator"));
    } else {
        g_variant_builder_add(&props, "{sv}", "label", g_variant_new_string(label));
        g_variant_builder_add(&props, "{sv}", "enabled", g_variant_new_boolean(TRUE));
        g_variant_builder_add(&props, "{sv}", "visible", g_variant_new_boolean(TRUE));
        if (checkable) {
            g_variant_builder_add(&props, "{sv}", "toggle-type", g_variant_new_string("checkmark"));
            g_variant_builder_add(&props, "{sv}", "toggle-state", g_variant_new_int32(checked ? 1 : 0));
        }
    }
    GVariantBuilder children;
    g_variant_builder_init(&children, G_VARIANT_TYPE("av"));
    return g_variant_new("(i@a{sv}@av)", id, g_variant_builder_end(&props), g_variant_builder_end(&children));
}

static void zero_native_sni_method(GDBusConnection *conn, const char *sender, const char *path, const char *iface, const char *method, GVariant *params, GDBusMethodInvocation *invocation, gpointer data) {
    (void)conn; (void)sender; (void)path; (void)iface; (void)params;
    zero_native_gtk_host_t *host = data;
    if (g_strcmp0(method, "Activate") == 0 || g_strcmp0(method, "SecondaryActivate") == 0) {
        zero_native_tray_toggle_window(host);
    }
    g_dbus_method_invocation_return_value(invocation, NULL);
}

static GVariant *zero_native_sni_get_property(GDBusConnection *conn, const char *sender, const char *path, const char *iface, const char *name, GError **error, gpointer data) {
    (void)conn; (void)sender; (void)path; (void)iface; (void)error;
    zero_native_gtk_host_t *host = data;
    if (g_strcmp0(name, "Category") == 0) return g_variant_new_string("Communications");
    if (g_strcmp0(name, "Id") == 0) return g_variant_new_string(host->bundle_id ? host->bundle_id : "zero-native");
    if (g_strcmp0(name, "Title") == 0) return g_variant_new_string(host->app_name ? host->app_name : "zero-native");
    if (g_strcmp0(name, "Status") == 0) return g_variant_new_string(host->tray_attention ? "NeedsAttention" : "Active");
    if (g_strcmp0(name, "IconName") == 0) return g_variant_new_string(host->bundle_id ? host->bundle_id : "");
    if (g_strcmp0(name, "AttentionIconName") == 0) return g_variant_new_string(host->bundle_id ? host->bundle_id : "");
    if (g_strcmp0(name, "OverlayIconName") == 0) return g_variant_new_string("");
    if (g_strcmp0(name, "IconPixmap") == 0) return zero_native_tray_icon_pixmap(host->icon_path);
    if (g_strcmp0(name, "ItemIsMenu") == 0) return g_variant_new_boolean(FALSE);
    if (g_strcmp0(name, "Menu") == 0) return g_variant_new_object_path("/MenuBar");
    if (g_strcmp0(name, "ToolTip") == 0) {
        const char *tip = host->tray_tooltip ? host->tray_tooltip : (host->app_name ? host->app_name : "");
        GVariant *empty = g_variant_new_array(G_VARIANT_TYPE("(iiay)"), NULL, 0);
        return g_variant_new("(s@a(iiay)ss)", "", empty, tip, "");
    }
    return NULL;
}

static const GDBusInterfaceVTable zero_native_sni_vtable = {
    zero_native_sni_method, zero_native_sni_get_property, NULL, { 0 }
};

static void zero_native_dbusmenu_method(GDBusConnection *conn, const char *sender, const char *path, const char *iface, const char *method, GVariant *params, GDBusMethodInvocation *invocation, gpointer data) {
    (void)conn; (void)sender; (void)path; (void)iface;
    zero_native_gtk_host_t *host = data;
    if (g_strcmp0(method, "GetLayout") == 0) {
        GVariantBuilder root_props;
        g_variant_builder_init(&root_props, G_VARIANT_TYPE("a{sv}"));
        g_variant_builder_add(&root_props, "{sv}", "children-display", g_variant_new_string("submenu"));
        GVariantBuilder children;
        g_variant_builder_init(&children, G_VARIANT_TYPE("av"));
        g_variant_builder_add(&children, "v", zero_native_menu_item_node(ZERO_NATIVE_TRAY_ITEM_SHOW, "Show / Hide ZeroWhats", 0, 0, 0));
        g_variant_builder_add(&children, "v", zero_native_menu_item_node(ZERO_NATIVE_TRAY_ITEM_MUTE, "Mute notifications", 1, host->muted, 0));
        g_variant_builder_add(&children, "v", zero_native_menu_item_node(ZERO_NATIVE_TRAY_ITEM_AUTOSTART, "Start on boot", 1, zero_native_autostart_enabled(host), 0));
        g_variant_builder_add(&children, "v", zero_native_menu_item_node(ZERO_NATIVE_TRAY_ITEM_LOCK, "Lock now", 0, 0, 0));
        g_variant_builder_add(&children, "v", zero_native_menu_item_node(0, "", 0, 0, 1));
        g_variant_builder_add(&children, "v", zero_native_menu_item_node(ZERO_NATIVE_TRAY_ITEM_QUIT, "Quit", 0, 0, 0));
        GVariant *root = g_variant_new("(i@a{sv}@av)", 0, g_variant_builder_end(&root_props), g_variant_builder_end(&children));
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(u@(ia{sv}av))", 1u, root));
        return;
    }
    if (g_strcmp0(method, "Event") == 0) {
        gint32 id = 0; const char *event_id = NULL;
        g_variant_get(params, "(i&svu)", &id, &event_id, NULL, NULL);
        if (g_strcmp0(event_id, "clicked") == 0) zero_native_tray_invoke(host, (guint32)id);
        g_dbus_method_invocation_return_value(invocation, NULL);
        return;
    }
    if (g_strcmp0(method, "AboutToShow") == 0) {
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(b)", FALSE));
        return;
    }
    if (g_strcmp0(method, "GetGroupProperties") == 0) {
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(@a(ia{sv}))", g_variant_new_array(G_VARIANT_TYPE("(ia{sv})"), NULL, 0)));
        return;
    }
    if (g_strcmp0(method, "GetProperty") == 0) {
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(v)", g_variant_new_string("")));
        return;
    }
    g_dbus_method_invocation_return_value(invocation, NULL);
}

static GVariant *zero_native_dbusmenu_get_property(GDBusConnection *conn, const char *sender, const char *path, const char *iface, const char *name, GError **error, gpointer data) {
    (void)conn; (void)sender; (void)path; (void)iface; (void)error; (void)data;
    if (g_strcmp0(name, "Version") == 0) return g_variant_new_uint32(3);
    if (g_strcmp0(name, "Status") == 0) return g_variant_new_string("normal");
    if (g_strcmp0(name, "TextDirection") == 0) return g_variant_new_string("ltr");
    if (g_strcmp0(name, "IconThemePath") == 0) return g_variant_new_array(G_VARIANT_TYPE_STRING, NULL, 0);
    return NULL;
}

static const GDBusInterfaceVTable zero_native_dbusmenu_vtable = {
    zero_native_dbusmenu_method, zero_native_dbusmenu_get_property, NULL, { 0 }
};

static void zero_native_sni_registered(GObject *source, GAsyncResult *res, gpointer data) {
    (void)data;
    GError *err = NULL;
    GVariant *ret = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &err);
    if (ret) g_variant_unref(ret);
    if (err) g_error_free(err);
}

static void zero_native_tray_register(zero_native_gtk_host_t *host) {
    GDBusConnection *bus = zero_native_host_bus(host);
    if (!bus || host->tray_sni_reg_id) return;
    if (!zero_native_sni_node) zero_native_sni_node = g_dbus_node_info_new_for_xml(zero_native_sni_xml, NULL);
    if (!zero_native_dbusmenu_node) zero_native_dbusmenu_node = g_dbus_node_info_new_for_xml(zero_native_dbusmenu_xml, NULL);
    if (!zero_native_sni_node || !zero_native_dbusmenu_node) return;

    host->tray_sni_reg_id = g_dbus_connection_register_object(
        bus, "/StatusNotifierItem", zero_native_sni_node->interfaces[0],
        &zero_native_sni_vtable, host, NULL, NULL);
    host->tray_menu_reg_id = g_dbus_connection_register_object(
        bus, "/MenuBar", zero_native_dbusmenu_node->interfaces[0],
        &zero_native_dbusmenu_vtable, host, NULL, NULL);

    g_dbus_connection_call(bus,
        "org.kde.StatusNotifierWatcher", "/StatusNotifierWatcher",
        "org.kde.StatusNotifierWatcher", "RegisterStatusNotifierItem",
        g_variant_new("(s)", g_dbus_connection_get_unique_name(bus)),
        NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, zero_native_sni_registered, host);
}

static void zero_native_tray_emit(zero_native_gtk_host_t *host, const char *signal_name) {
    if (!host->bus) return;
    g_dbus_connection_emit_signal(host->bus, NULL, "/StatusNotifierItem", "org.kde.StatusNotifierItem", signal_name, NULL, NULL);
}

// Public entry point: create the tray on first call, then update tooltip / attention.
void zero_native_gtk_tray_set(zero_native_gtk_host_t *host, const char *tooltip, size_t tooltip_len, int attention) {
    if (!host) return;
    int new_attention = attention ? 1 : 0;
    const char *old_tooltip = host->tray_tooltip ? host->tray_tooltip : "";
    int tooltip_changed = strncmp(old_tooltip, tooltip ? tooltip : "", tooltip_len) != 0 ||
        strlen(old_tooltip) != tooltip_len;
    int attention_changed = host->tray_attention != new_attention;

    free(host->tray_tooltip);
    host->tray_tooltip = tooltip_len > 0 ? zero_native_strndup(tooltip, tooltip_len) : NULL;
    host->tray_attention = new_attention;

    if (!host->tray_active) {
        zero_native_tray_register(host);
        host->tray_active = 1;
        return;
    }
    // Only signal the host when something actually changed, to avoid churn.
    if (tooltip_changed) zero_native_tray_emit(host, "NewToolTip");
    if (attention_changed) {
        g_dbus_connection_emit_signal(host->bus, NULL, "/StatusNotifierItem", "org.kde.StatusNotifierItem",
            "NewStatus", g_variant_new("(s)", host->tray_attention ? "NeedsAttention" : "Active"), NULL);
    }
}

// Lazily create the shared, persistent network session. Cookies and the rest
// of the website data are stored on disk so logins persist across restarts.
static WebKitNetworkSession *zero_native_host_session(zero_native_gtk_host_t *host) {
    if (host->session) return host->session;

    char *data_dir = host->data_dir
        ? g_strdup(host->data_dir)
        : g_build_filename(g_get_user_data_dir(), host->bundle_id ? host->bundle_id : "zero-native", NULL);
    char *cache_dir = host->cache_dir
        ? g_strdup(host->cache_dir)
        : g_build_filename(g_get_user_cache_dir(), host->bundle_id ? host->bundle_id : "zero-native", NULL);
    g_mkdir_with_parents(data_dir, 0700);
    g_mkdir_with_parents(cache_dir, 0700);

    host->session = webkit_network_session_new(data_dir, cache_dir);
    if (host->session) {
        WebKitCookieManager *cookies = webkit_network_session_get_cookie_manager(host->session);
        char *cookie_file = g_build_filename(data_dir, "cookies.sqlite", NULL);
        webkit_cookie_manager_set_persistent_storage(cookies, cookie_file, WEBKIT_COOKIE_PERSISTENT_STORAGE_SQLITE);
        webkit_cookie_manager_set_accept_policy(cookies, WEBKIT_COOKIE_POLICY_ACCEPT_ALWAYS);
        g_free(cookie_file);
    }
    g_free(data_dir);
    g_free(cache_dir);
    return host->session;
}

// Apply the configured user agent and enable the web features WhatsApp Web
// expects (media stream for calls, etc.).
static void zero_native_apply_webview_settings(zero_native_gtk_host_t *host, WebKitWebView *wv) {
    WebKitSettings *settings = webkit_web_view_get_settings(wv);
    if (!settings) return;
    if (host->user_agent && host->user_agent[0]) {
        webkit_settings_set_user_agent(settings, host->user_agent);
    }
    webkit_settings_set_enable_media_stream(settings, TRUE);
    webkit_settings_set_enable_mediasource(settings, TRUE);
    webkit_settings_set_enable_webrtc(settings, TRUE);
    webkit_settings_set_javascript_can_access_clipboard(settings, TRUE);
    webkit_settings_set_enable_page_cache(settings, TRUE);
    // Opt-in JS console -> stdout for debugging (ZERO_NATIVE_DEBUG_CONSOLE=1).
    if (g_getenv("ZERO_NATIVE_DEBUG_CONSOLE")) {
        webkit_settings_set_enable_write_console_messages_to_stdout(settings, TRUE);
    }
}

static zero_native_gtk_window_t *zero_native_create_window_internal(zero_native_gtk_host_t *host, uint64_t window_id, const char *title, const char *label, double x, double y, double width, double height, int restore_frame, int decorated) {
    if (zero_native_find_window(host, window_id)) return NULL;

    int slot = -1;
    for (int i = 0; i < host->window_count; i++) {
        if (!host->windows[i].gtk_window) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        if (host->window_count >= ZERO_NATIVE_MAX_WINDOWS) return NULL;
        slot = host->window_count++;
    }

    zero_native_gtk_window_t *win = &host->windows[slot];
    memset(win, 0, sizeof(*win));
    win->id = window_id;
    win->host = host;
    win->x = restore_frame ? x : 0;
    win->y = restore_frame ? y : 0;
    win->label = zero_native_strndup(label && label[0] ? label : "main", strlen(label && label[0] ? label : "main"));
    win->title = zero_native_strndup(title && title[0] ? title : host->app_name, strlen(title && title[0] ? title : host->app_name));
    if (!win->label || !win->title) {
        free(win->label);
        free(win->title);
        memset(win, 0, sizeof(*win));
        return NULL;
    }

    win->gtk_window = GTK_WINDOW(gtk_application_window_new(host->app));
    gtk_window_set_title(win->gtk_window, win->title);
    gtk_window_set_default_size(win->gtk_window, (int)width, (int)height);
    // ZeroWhats: frameless windows for custom client-side title bars.
    gtk_window_set_decorated(win->gtk_window, decorated ? TRUE : FALSE);

    win->content_manager = webkit_user_content_manager_new();
    WebKitWebView *wv = WEBKIT_WEB_VIEW(
        g_object_new(WEBKIT_TYPE_WEB_VIEW,
            "user-content-manager", win->content_manager,
            "network-session", zero_native_host_session(host),
            NULL));
    win->web_view = wv;
    zero_native_apply_webview_settings(host, wv);
    zero_native_connect_webview_signals(win, wv);
    if (!host->scheme_registered) {
        WebKitWebContext *context = webkit_web_view_get_context(wv);
        webkit_web_context_register_uri_scheme(context, "zero", zero_native_asset_scheme_request, host, NULL);
        // Treat app-local content as a secure context so it can use service
        // workers, the Notification API and other secure-only web features.
        WebKitSecurityManager *security = webkit_web_context_get_security_manager(context);
        webkit_security_manager_register_uri_scheme_as_secure(security, "zero");
        host->scheme_registered = 1;
    }
    zero_native_setup_bridge(win);

    win->stack_root = gtk_overlay_new();
    gtk_overlay_set_child(GTK_OVERLAY(win->stack_root), GTK_WIDGET(wv));
    gtk_window_set_child(win->gtk_window, win->stack_root);

    g_signal_connect(win->gtk_window, "notify::default-width", G_CALLBACK(on_resize), win);
    g_signal_connect(win->gtk_window, "notify::default-height", G_CALLBACK(on_resize), win);
    g_signal_connect(win->gtk_window, "notify::is-active", G_CALLBACK(on_focus), win);
    g_signal_connect(win->gtk_window, "close-request", G_CALLBACK(on_close_request), win);
    g_signal_connect(win->web_view, "decide-policy", G_CALLBACK(on_decide_policy), win);

    return win;
}

static void on_activate(GtkApplication *app, gpointer data) {
    (void)app;
    zero_native_gtk_host_t *host = data;

    zero_native_gtk_window_t *win = zero_native_create_window_internal(
        host, 1, host->window_title, host->window_label,
        host->init_x, host->init_y,
        host->init_width > 0 ? host->init_width : 720,
        host->init_height > 0 ? host->init_height : 480,
        host->restore_frame, host->decorated);
    if (!win) return;

    gtk_window_present(win->gtk_window);

    zero_native_emit(host, (zero_native_gtk_event_t){ .kind = ZERO_NATIVE_GTK_EVENT_START });
    zero_native_emit_resize(host, win);
    zero_native_emit_window_frame(host, win, 1);

    // The runtime only acts on a frame when state is invalidated (user driven),
    // and WebKit repaints the content itself, so a low-frequency tick keeps idle
    // CPU/power minimal without affecting responsiveness.
    host->frame_timer = g_timeout_add(100, zero_native_frame_tick, host);
}

zero_native_gtk_host_t *zero_native_gtk_create(
    const char *app_name, size_t app_name_len,
    const char *window_title, size_t window_title_len,
    const char *bundle_id, size_t bundle_id_len,
    const char *icon_path, size_t icon_path_len,
    const char *window_label, size_t window_label_len,
    double x, double y, double width, double height,
    int restore_frame, int decorated)
{
    zero_native_gtk_host_t *host = calloc(1, sizeof(zero_native_gtk_host_t));
    if (!host) return NULL;

    host->app_name = app_name_len > 0 ? zero_native_strndup(app_name, app_name_len) : zero_native_strndup("zero-native", 11);
    host->window_title = window_title_len > 0 ? zero_native_strndup(window_title, window_title_len) : zero_native_strndup(host->app_name, strlen(host->app_name));
    host->bundle_id = bundle_id_len > 0 ? zero_native_strndup(bundle_id, bundle_id_len) : zero_native_strndup("dev.zero_native.app", 19);
    host->icon_path = icon_path_len > 0 ? zero_native_strndup(icon_path, icon_path_len) : NULL;
    host->window_label = window_label_len > 0 ? zero_native_strndup(window_label, window_label_len) : zero_native_strndup("main", 4);
    host->init_x = x;
    host->init_y = y;
    host->init_width = width;
    host->init_height = height;
    host->restore_frame = restore_frame;

    host->allowed_origins = NULL;
    host->allowed_origins_count = 0;
    host->allowed_external_urls = NULL;
    host->allowed_external_urls_count = 0;

    host->app = gtk_application_new(host->bundle_id, G_APPLICATION_DEFAULT_FLAGS);

    return host;
}

// Configure the persistent storage location and user agent for every web view.
// Must be called before the windows are created (i.e. before run). Empty strings
// fall back to per-bundle defaults under the XDG data/cache directories.
void zero_native_gtk_configure_webview_session(zero_native_gtk_host_t *host, const char *data_dir, size_t data_dir_len, const char *cache_dir, size_t cache_dir_len, const char *user_agent, size_t user_agent_len) {
    if (!host) return;
    free(host->data_dir);
    free(host->cache_dir);
    free(host->user_agent);
    host->data_dir = data_dir_len > 0 ? zero_native_strndup(data_dir, data_dir_len) : NULL;
    host->cache_dir = cache_dir_len > 0 ? zero_native_strndup(cache_dir, cache_dir_len) : NULL;
    host->user_agent = user_agent_len > 0 ? zero_native_strndup(user_agent, user_agent_len) : NULL;
}

void zero_native_gtk_destroy(zero_native_gtk_host_t *host) {
    if (!host) return;
    if (host->frame_timer) g_source_remove(host->frame_timer);
    for (int i = 0; i < host->window_count; i++) {
        zero_native_clear_window(&host->windows[i]);
    }
    g_object_unref(host->app);
    for (int i = 0; i < host->notification_count; i++) {
        if (host->notifications[i].notification) g_object_unref(host->notifications[i].notification);
    }
    if (host->bus) {
        if (host->notif_action_sub) g_dbus_connection_signal_unsubscribe(host->bus, host->notif_action_sub);
        if (host->notif_closed_sub) g_dbus_connection_signal_unsubscribe(host->bus, host->notif_closed_sub);
        if (host->tray_sni_reg_id) g_dbus_connection_unregister_object(host->bus, host->tray_sni_reg_id);
        if (host->tray_menu_reg_id) g_dbus_connection_unregister_object(host->bus, host->tray_menu_reg_id);
        g_object_unref(host->bus);
    }
    free(host->tray_tooltip);
    if (host->session) g_object_unref(host->session);
    free(host->app_name);
    free(host->window_title);
    free(host->bundle_id);
    free(host->icon_path);
    free(host->window_label);
    free(host->data_dir);
    free(host->cache_dir);
    free(host->user_agent);
    zero_native_free_string_list(host->allowed_origins, host->allowed_origins_count);
    zero_native_free_string_list(host->allowed_external_urls, host->allowed_external_urls_count);
    free(host);
}

void zero_native_gtk_run(zero_native_gtk_host_t *host, zero_native_gtk_event_callback_t callback, void *context) {
    host->callback = callback;
    host->callback_context = context;
    g_signal_connect(host->app, "activate", G_CALLBACK(on_activate), host);
    g_application_run(G_APPLICATION(host->app), 0, NULL);
}

void zero_native_gtk_stop(zero_native_gtk_host_t *host) {
    if (!host->did_shutdown) {
        host->did_shutdown = 1;
        zero_native_emit(host, (zero_native_gtk_event_t){ .kind = ZERO_NATIVE_GTK_EVENT_SHUTDOWN });
    }
    if (host->frame_timer) {
        g_source_remove(host->frame_timer);
        host->frame_timer = 0;
    }
    g_application_quit(G_APPLICATION(host->app));
}

void zero_native_gtk_load_webview(zero_native_gtk_host_t *host, const char *source, size_t source_len, int source_kind, const char *asset_root, size_t asset_root_len, const char *asset_entry, size_t asset_entry_len, const char *asset_origin, size_t asset_origin_len, int spa_fallback) {
    zero_native_gtk_load_window_webview(host, 1, source, source_len, source_kind, asset_root, asset_root_len, asset_entry, asset_entry_len, asset_origin, asset_origin_len, spa_fallback);
}

void zero_native_gtk_load_window_webview(zero_native_gtk_host_t *host, uint64_t window_id, const char *source, size_t source_len, int source_kind, const char *asset_root, size_t asset_root_len, const char *asset_entry, size_t asset_entry_len, const char *asset_origin, size_t asset_origin_len, int spa_fallback) {
    zero_native_gtk_window_t *win = zero_native_find_window(host, window_id);
    if (!win || !win->web_view) return;

    char *src = zero_native_strndup(source, source_len);
    if (!src) return;

    zero_native_clear_window_source(win);
    if (source_kind == 1) {
        webkit_web_view_load_uri(win->web_view, src);
    } else if (source_kind == 2) {
        char *root = asset_root_len > 0 ? zero_native_strndup(asset_root, asset_root_len) : zero_native_strndup(".", 1);
        char *entry = asset_entry_len > 0 ? zero_native_strndup(asset_entry, asset_entry_len) : zero_native_strndup("index.html", strlen("index.html"));
        char *origin = asset_origin_len > 0 ? zero_native_strndup(asset_origin, asset_origin_len) : zero_native_strndup("zero://app", strlen("zero://app"));
        if (!root || !entry || !origin) {
            free(root);
            free(entry);
            free(origin);
            free(src);
            return;
        }
        while (entry[0] == '/') memmove(entry, entry + 1, strlen(entry));
        if (!zero_native_path_is_safe(entry)) {
            free(entry);
            entry = zero_native_strndup("index.html", strlen("index.html"));
        }
        char *canonical_root = g_canonicalize_filename(root, NULL);
        free(root);
        win->asset_root = canonical_root ? zero_native_strndup(canonical_root, strlen(canonical_root)) : zero_native_strndup(".", 1);
        g_free(canonical_root);
        win->asset_entry = entry;
        win->asset_origin = origin;
        win->bridge_origin = zero_native_strndup(origin, strlen(origin));
        win->spa_fallback = spa_fallback != 0;

        char *uri = g_strdup_printf("%s/%s", origin, entry);
        webkit_web_view_load_uri(win->web_view, uri);
        g_free(uri);
    } else {
        win->bridge_origin = zero_native_strndup("zero://inline", strlen("zero://inline"));
        webkit_web_view_load_html(win->web_view, src, "zero://inline");
    }
    free(src);
}

void zero_native_gtk_set_bridge_callback(zero_native_gtk_host_t *host, zero_native_gtk_bridge_callback_t callback, void *context) {
    host->bridge_callback = callback;
    host->bridge_context = context;
}

void zero_native_gtk_bridge_respond(zero_native_gtk_host_t *host, const char *response, size_t response_len) {
    zero_native_gtk_bridge_respond_window(host, 1, response, response_len);
}

void zero_native_gtk_bridge_respond_window(zero_native_gtk_host_t *host, uint64_t window_id, const char *response, size_t response_len) {
    zero_native_gtk_bridge_respond_webview(host, window_id, "main", 4, response, response_len);
}

void zero_native_gtk_bridge_respond_webview(zero_native_gtk_host_t *host, uint64_t window_id, const char *webview_label, size_t webview_label_len, const char *response, size_t response_len) {
    zero_native_gtk_window_t *win = zero_native_find_window(host, window_id);
    if (!win || !win->web_view) return;
    char *label = webview_label_len > 0 ? zero_native_strndup(webview_label, webview_label_len) : zero_native_strndup("main", 4);
    if (!label) return;
    WebKitWebView *target = NULL;
    if (strcmp(label, "main") == 0) {
        target = win->web_view;
    } else {
        zero_native_gtk_webview_t *webview = zero_native_find_webview(win, label);
        if (webview) target = webview->web_view;
    }
    free(label);
    if (!target) return;

    char *resp = zero_native_strndup(response, response_len);
    if (!resp) return;

    size_t prefix_len = strlen("window.zero&&window.zero._complete(");
    size_t suffix_len = 2; /* ); */
    size_t script_len = prefix_len + response_len + suffix_len;
    char *script = malloc(script_len + 1);
    if (script) {
        memcpy(script, "window.zero&&window.zero._complete(", prefix_len);
        memcpy(script + prefix_len, resp, response_len);
        memcpy(script + prefix_len + response_len, ");", suffix_len);
        script[script_len] = '\0';
        webkit_web_view_evaluate_javascript(target, script, -1, NULL, NULL, NULL, NULL, NULL);
        free(script);
    }
    free(resp);
}

void zero_native_gtk_emit_window_event(zero_native_gtk_host_t *host, uint64_t window_id, const char *name, size_t name_len, const char *detail_json, size_t detail_json_len) {
    zero_native_gtk_window_t *win = zero_native_find_window(host, window_id);
    if (!win || !win->web_view) return;
    char *event_name = zero_native_strndup(name, name_len);
    char *detail = zero_native_strndup(detail_json, detail_json_len);
    if (!event_name || !detail) {
        free(event_name);
        free(detail);
        return;
    }
    GString *script = g_string_new("window.zero&&window.zero._emit(");
    g_string_append_c(script, '"');
    for (const char *p = event_name; *p; p++) {
        if (*p == '"' || *p == '\\') g_string_append_c(script, '\\');
        g_string_append_c(script, *p);
    }
    g_string_append(script, "\",");
    g_string_append(script, detail[0] ? detail : "null");
    g_string_append(script, ");");
    webkit_web_view_evaluate_javascript(win->web_view, script->str, -1, NULL, NULL, NULL, NULL, NULL);
    g_string_free(script, TRUE);
    free(event_name);
    free(detail);
}

void zero_native_gtk_set_security_policy(zero_native_gtk_host_t *host, const char *allowed_origins, size_t allowed_origins_len, const char *external_urls, size_t external_urls_len, int external_action) {
    zero_native_free_string_list(host->allowed_origins, host->allowed_origins_count);
    zero_native_free_string_list(host->allowed_external_urls, host->allowed_external_urls_count);
    host->allowed_origins = zero_native_parse_newline_list(allowed_origins, allowed_origins_len, &host->allowed_origins_count);
    host->allowed_external_urls = zero_native_parse_newline_list(external_urls, external_urls_len, &host->allowed_external_urls_count);
    host->external_link_action = external_action;
}

int zero_native_gtk_create_window(zero_native_gtk_host_t *host, uint64_t window_id, const char *window_title, size_t window_title_len, const char *window_label, size_t window_label_len, double x, double y, double width, double height, int restore_frame, int decorated) {
    char *title = window_title_len > 0 ? zero_native_strndup(window_title, window_title_len) : NULL;
    char *label = window_label_len > 0 ? zero_native_strndup(window_label, window_label_len) : NULL;
    zero_native_gtk_window_t *win = zero_native_create_window_internal(host, window_id, title, label, x, y, width, height, restore_frame, decorated);
    free(title);
    free(label);
    if (!win) return 0;

    gtk_window_present(win->gtk_window);
    return 1;
}

int zero_native_gtk_focus_window(zero_native_gtk_host_t *host, uint64_t window_id) {
    zero_native_gtk_window_t *win = zero_native_find_window(host, window_id);
    if (!win || !win->gtk_window) return 0;
    gtk_window_present(win->gtk_window);
    zero_native_emit_window_frame(host, win, 1);
    return 1;
}

int zero_native_gtk_close_window(zero_native_gtk_host_t *host, uint64_t window_id) {
    zero_native_gtk_window_t *win = zero_native_find_window(host, window_id);
    if (!win || !win->gtk_window) return 0;
    gtk_window_close(win->gtk_window);
    return 1;
}

int zero_native_gtk_set_window_decorated(zero_native_gtk_host_t *host, uint64_t window_id, int decorated) {
    zero_native_gtk_window_t *win = zero_native_find_window(host, window_id);
    if (!win || !win->gtk_window) return 0;
    gtk_window_set_decorated(win->gtk_window, decorated ? TRUE : FALSE);
    return 1;
}

int zero_native_gtk_minimize_window(zero_native_gtk_host_t *host, uint64_t window_id) {
    zero_native_gtk_window_t *win = zero_native_find_window(host, window_id);
    if (!win || !win->gtk_window) return 0;
    gtk_window_minimize(win->gtk_window);
    return 1;
}

int zero_native_gtk_toggle_maximize_window(zero_native_gtk_host_t *host, uint64_t window_id) {
    zero_native_gtk_window_t *win = zero_native_find_window(host, window_id);
    if (!win || !win->gtk_window) return 0;
    if (gtk_window_is_maximized(win->gtk_window)) {
        gtk_window_unmaximize(win->gtk_window);
    } else {
        gtk_window_maximize(win->gtk_window);
    }
    return 1;
}

// Hand the active pointer grab to the compositor for an interactive move/resize.
// `resize` selects begin_resize; `edge` is a GdkSurfaceEdge value (ignored for move).
static int zero_native_begin_window_drag(zero_native_gtk_window_t *win, int resize, int edge) {
    if (!win || !win->gtk_window) return 0;
    GtkNative *native = GTK_NATIVE(win->gtk_window);
    if (!native) return 0;
    GdkSurface *surface = gtk_native_get_surface(native);
    if (!surface || !GDK_IS_TOPLEVEL(surface)) return 0;
    GdkDisplay *display = gtk_widget_get_display(GTK_WIDGET(win->gtk_window));
    GdkSeat *seat = gdk_display_get_default_seat(display);
    GdkDevice *device = seat ? gdk_seat_get_pointer(seat) : NULL;
    if (!device) return 0;
    double x = 0, y = 0;
    GdkModifierType mask = 0;
    gdk_surface_get_device_position(surface, device, &x, &y, &mask);
    if (resize) {
        gdk_toplevel_begin_resize(GDK_TOPLEVEL(surface), (GdkSurfaceEdge)edge, device, GDK_BUTTON_PRIMARY, x, y, GDK_CURRENT_TIME);
    } else {
        gdk_toplevel_begin_move(GDK_TOPLEVEL(surface), device, GDK_BUTTON_PRIMARY, x, y, GDK_CURRENT_TIME);
    }
    return 1;
}

int zero_native_gtk_start_window_drag(zero_native_gtk_host_t *host, uint64_t window_id) {
    return zero_native_begin_window_drag(zero_native_find_window(host, window_id), 0, 0);
}

int zero_native_gtk_start_window_resize(zero_native_gtk_host_t *host, uint64_t window_id, int edge) {
    return zero_native_begin_window_drag(zero_native_find_window(host, window_id), 1, edge);
}

int zero_native_gtk_create_webview(zero_native_gtk_host_t *host, uint64_t window_id, const char *label, size_t label_len, const char *url, size_t url_len, double x, double y, double width, double height, int layer, int transparent, int bridge_enabled) {
    zero_native_gtk_window_t *win = zero_native_find_window(host, window_id);
    if (!win || !win->stack_root || label_len == 0 || url_len == 0 || !zero_native_valid_webview_frame(x, y, width, height)) return 0;
    if (win->webview_count >= ZERO_NATIVE_MAX_WEBVIEWS) return 0;

    char *label_copy = zero_native_strndup(label, label_len);
    char *url_copy = zero_native_strndup(url, url_len);
    if (!label_copy || !url_copy) {
        free(label_copy);
        free(url_copy);
        return 0;
    }
    if (zero_native_find_webview(win, label_copy) || !zero_native_policy_list_matches(host->allowed_origins, host->allowed_origins_count, url_copy)) {
        free(label_copy);
        free(url_copy);
        return 0;
    }

    WebKitUserContentManager *manager = webkit_user_content_manager_new();
    if (bridge_enabled) {
        g_signal_connect(manager, "script-message-received::zeroNativeBridge", G_CALLBACK(on_bridge_message), win);
        webkit_user_content_manager_register_script_message_handler(manager, "zeroNativeBridge", NULL);
        WebKitUserScript *script = webkit_user_script_new(
            zero_native_bridge_script(),
            WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
            WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START,
            NULL, NULL);
        webkit_user_content_manager_add_script(manager, script);
        webkit_user_script_unref(script);
    }
    WebKitWebView *web_view = WEBKIT_WEB_VIEW(
        g_object_new(WEBKIT_TYPE_WEB_VIEW,
            "user-content-manager", manager,
            "network-session", zero_native_host_session(win->host),
            NULL));
    g_object_unref(manager);
    if (!web_view) {
        free(label_copy);
        free(url_copy);
        return 0;
    }
    zero_native_apply_webview_settings(win->host, web_view);
    zero_native_connect_webview_signals(win, web_view);

    zero_native_gtk_webview_t *webview = &win->webviews[win->webview_count++];
    memset(webview, 0, sizeof(*webview));
    webview->label = label_copy;
    webview->web_view = web_view;
    webview->x = x;
    webview->y = y;
    webview->width = width;
    webview->height = height;
    webview->layer = layer;
    webview->transparent = transparent != 0;
    webview->bridge_enabled = bridge_enabled != 0;
    webview->content_manager = manager;
    zero_native_apply_webview_frame(webview);
    gtk_overlay_add_overlay(GTK_OVERLAY(win->stack_root), GTK_WIDGET(web_view));
    // Do not let the (large) child web view drive the toplevel size — otherwise
    // its min content width inflates the window on every resize cycle.
    gtk_overlay_set_measure_overlay(GTK_OVERLAY(win->stack_root), GTK_WIDGET(web_view), FALSE);
    if (transparent) {
        GdkRGBA transparent_color = {0, 0, 0, 0};
        webkit_web_view_set_background_color(web_view, &transparent_color);
    }
    zero_native_reorder_webviews(win);
    g_signal_connect(web_view, "decide-policy", G_CALLBACK(on_webview_decide_policy), win);
    webkit_web_view_load_uri(web_view, url_copy);
    free(url_copy);
    return 1;
}

int zero_native_gtk_set_webview_frame(zero_native_gtk_host_t *host, uint64_t window_id, const char *label, size_t label_len, double x, double y, double width, double height) {
    zero_native_gtk_window_t *win = zero_native_find_window(host, window_id);
    char *label_copy = label_len > 0 ? zero_native_strndup(label, label_len) : NULL;
    if (label_copy && strcmp(label_copy, "main") == 0 && win && win->web_view && zero_native_valid_webview_frame(x, y, width, height)) {
        GtkWidget *widget = GTK_WIDGET(win->web_view);
        gtk_widget_set_halign(widget, GTK_ALIGN_START);
        gtk_widget_set_valign(widget, GTK_ALIGN_START);
        gtk_widget_set_margin_start(widget, zero_native_webview_coord(x));
        gtk_widget_set_margin_top(widget, zero_native_webview_coord(y));
        gtk_widget_set_size_request(widget, zero_native_webview_extent(width), zero_native_webview_extent(height));
        free(label_copy);
        return 1;
    }
    zero_native_gtk_webview_t *webview = zero_native_find_webview(win, label_copy);
    free(label_copy);
    if (!webview || !zero_native_valid_webview_frame(x, y, width, height)) return 0;
    webview->x = x;
    webview->y = y;
    webview->width = width;
    webview->height = height;
    zero_native_apply_webview_frame(webview);
    return 1;
}

int zero_native_gtk_navigate_webview(zero_native_gtk_host_t *host, uint64_t window_id, const char *label, size_t label_len, const char *url, size_t url_len) {
    zero_native_gtk_window_t *win = zero_native_find_window(host, window_id);
    char *label_copy = label_len > 0 ? zero_native_strndup(label, label_len) : NULL;
    char *url_copy = url_len > 0 ? zero_native_strndup(url, url_len) : NULL;
    zero_native_gtk_webview_t *webview = zero_native_find_webview(win, label_copy);
    if (!webview || !url_copy || !zero_native_policy_list_matches(host->allowed_origins, host->allowed_origins_count, url_copy)) {
        free(label_copy);
        free(url_copy);
        return 0;
    }
    webkit_web_view_load_uri(webview->web_view, url_copy);
    free(label_copy);
    free(url_copy);
    return 1;
}

int zero_native_gtk_set_webview_zoom(zero_native_gtk_host_t *host, uint64_t window_id, const char *label, size_t label_len, double zoom) {
    zero_native_gtk_window_t *win = zero_native_find_window(host, window_id);
    char *label_copy = label_len > 0 ? zero_native_strndup(label, label_len) : NULL;
    if (label_copy && strcmp(label_copy, "main") == 0 && win && win->web_view && zoom >= 0.25 && zoom <= 5.0) {
        webkit_web_view_set_zoom_level(win->web_view, zoom);
        free(label_copy);
        return 1;
    }
    zero_native_gtk_webview_t *webview = zero_native_find_webview(win, label_copy);
    free(label_copy);
    if (!webview || !webview->web_view || zoom < 0.25 || zoom > 5.0) return 0;
    webkit_web_view_set_zoom_level(webview->web_view, zoom);
    return 1;
}

int zero_native_gtk_set_webview_layer(zero_native_gtk_host_t *host, uint64_t window_id, const char *label, size_t label_len, int layer) {
    zero_native_gtk_window_t *win = zero_native_find_window(host, window_id);
    char *label_copy = label_len > 0 ? zero_native_strndup(label, label_len) : NULL;
    if (label_copy && strcmp(label_copy, "main") == 0 && win && win->web_view) {
        free(label_copy);
        return 0;
    }
    zero_native_gtk_webview_t *webview = zero_native_find_webview(win, label_copy);
    free(label_copy);
    if (!webview || !webview->web_view) return 0;
    webview->layer = layer;
    zero_native_reorder_webviews(win);
    return 1;
}

int zero_native_gtk_close_webview(zero_native_gtk_host_t *host, uint64_t window_id, const char *label, size_t label_len) {
    zero_native_gtk_window_t *win = zero_native_find_window(host, window_id);
    char *label_copy = label_len > 0 ? zero_native_strndup(label, label_len) : NULL;
    if (!win || !label_copy) {
        free(label_copy);
        return 0;
    }
    for (int i = 0; i < win->webview_count; i++) {
        if (win->webviews[i].label && strcmp(win->webviews[i].label, label_copy) == 0) {
            free(label_copy);
            zero_native_remove_webview_at(win, i);
            return 1;
        }
    }
    free(label_copy);
    return 0;
}

typedef struct zero_native_clipboard_read_state {
    GMainLoop *loop;
    char *text;
} zero_native_clipboard_read_state_t;

static void zero_native_clipboard_read_done(GObject *source, GAsyncResult *result, gpointer data) {
    zero_native_clipboard_read_state_t *state = data;
    GError *error = NULL;
    state->text = gdk_clipboard_read_text_finish(GDK_CLIPBOARD(source), result, &error);
    if (error) g_error_free(error);
    g_main_loop_quit(state->loop);
}

size_t zero_native_gtk_clipboard_read(zero_native_gtk_host_t *host, char *buffer, size_t buffer_len) {
    (void)host;
    GdkDisplay *display = gdk_display_get_default();
    if (!display) return 0;
    GdkClipboard *clipboard = gdk_display_get_clipboard(display);
    if (!clipboard) return 0;
    zero_native_clipboard_read_state_t state = {0};
    state.loop = g_main_loop_new(NULL, FALSE);
    if (!state.loop) return 0;
    gdk_clipboard_read_text_async(clipboard, NULL, zero_native_clipboard_read_done, &state);
    g_main_loop_run(state.loop);
    g_main_loop_unref(state.loop);
    if (!state.text) return 0;
    size_t len = strlen(state.text);
    size_t count = len < buffer_len ? len : buffer_len;
    memcpy(buffer, state.text, count);
    g_free(state.text);
    return count;
}

void zero_native_gtk_clipboard_write(zero_native_gtk_host_t *host, const char *text, size_t text_len) {
    (void)host;
    GdkDisplay *display = gdk_display_get_default();
    if (!display) return;
    GdkClipboard *clipboard = gdk_display_get_clipboard(display);
    if (!clipboard) return;
    char *copy = zero_native_strndup(text, text_len);
    if (!copy) return;
    gdk_clipboard_set_text(clipboard, copy);
    free(copy);
}

typedef struct zero_native_file_dialog_state {
    GMainLoop *loop;
    GListModel *files;
    GFile *file;
} zero_native_file_dialog_state_t;

static GtkWindow *zero_native_parent_window(zero_native_gtk_host_t *host) {
    for (int i = 0; i < host->window_count; i++) {
        if (host->windows[i].gtk_window) return host->windows[i].gtk_window;
    }
    return NULL;
}

static char *zero_native_bytes_to_string(const char *bytes, size_t len) {
    return bytes && len > 0 ? zero_native_strndup(bytes, len) : NULL;
}

static void zero_native_open_dialog_done(GObject *source, GAsyncResult *result, gpointer data) {
    zero_native_file_dialog_state_t *state = data;
    GError *error = NULL;
    state->files = gtk_file_dialog_open_multiple_finish(GTK_FILE_DIALOG(source), result, &error);
    if (error) g_error_free(error);
    g_main_loop_quit(state->loop);
}

static void zero_native_folder_dialog_done(GObject *source, GAsyncResult *result, gpointer data) {
    zero_native_file_dialog_state_t *state = data;
    GError *error = NULL;
    state->files = gtk_file_dialog_select_multiple_folders_finish(GTK_FILE_DIALOG(source), result, &error);
    if (error) g_error_free(error);
    g_main_loop_quit(state->loop);
}

static void zero_native_save_dialog_done(GObject *source, GAsyncResult *result, gpointer data) {
    zero_native_file_dialog_state_t *state = data;
    GError *error = NULL;
    state->file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(source), result, &error);
    if (error) g_error_free(error);
    g_main_loop_quit(state->loop);
}

zero_native_gtk_open_dialog_result_t zero_native_gtk_show_open_dialog(zero_native_gtk_host_t *host, const zero_native_gtk_open_dialog_opts_t *opts, char *buffer, size_t buffer_len) {
    zero_native_gtk_open_dialog_result_t result = {0};
    GtkFileDialog *dialog = gtk_file_dialog_new();
    char *title = zero_native_bytes_to_string(opts->title, opts->title_len);
    if (title) gtk_file_dialog_set_title(dialog, title);
    GtkWindow *parent = zero_native_parent_window(host);
    zero_native_file_dialog_state_t state = { .loop = g_main_loop_new(NULL, FALSE) };
    if (!state.loop) {
        if (title) free(title);
        g_object_unref(dialog);
        return result;
    }
    if (opts->allow_directories) {
        gtk_file_dialog_select_multiple_folders(dialog, parent, NULL, zero_native_folder_dialog_done, &state);
    } else {
        gtk_file_dialog_open_multiple(dialog, parent, NULL, zero_native_open_dialog_done, &state);
    }
    g_main_loop_run(state.loop);
    g_main_loop_unref(state.loop);
    if (state.files) {
        size_t offset = 0;
        guint count = g_list_model_get_n_items(state.files);
        for (guint i = 0; i < count; i++) {
            GFile *file = G_FILE(g_list_model_get_item(state.files, i));
            char *path = g_file_get_path(file);
            if (path) {
                size_t len = strlen(path);
                size_t needed = len + (result.count > 0 ? 1 : 0);
                if (offset + needed <= buffer_len) {
                    if (result.count > 0) buffer[offset++] = '\n';
                    memcpy(buffer + offset, path, len);
                    offset += len;
                    result.count++;
                }
                g_free(path);
            }
            g_object_unref(file);
        }
        result.bytes_written = offset;
        g_object_unref(state.files);
    }
    if (title) free(title);
    g_object_unref(dialog);
    return result;
}

size_t zero_native_gtk_show_save_dialog(zero_native_gtk_host_t *host, const zero_native_gtk_save_dialog_opts_t *opts, char *buffer, size_t buffer_len) {
    GtkFileDialog *dialog = gtk_file_dialog_new();
    char *title = zero_native_bytes_to_string(opts->title, opts->title_len);
    char *default_name = zero_native_bytes_to_string(opts->default_name, opts->default_name_len);
    if (title) gtk_file_dialog_set_title(dialog, title);
    if (default_name) gtk_file_dialog_set_initial_name(dialog, default_name);
    GtkWindow *parent = zero_native_parent_window(host);
    zero_native_file_dialog_state_t state = { .loop = g_main_loop_new(NULL, FALSE) };
    if (!state.loop) {
        if (title) free(title);
        if (default_name) free(default_name);
        g_object_unref(dialog);
        return 0;
    }
    gtk_file_dialog_save(dialog, parent, NULL, zero_native_save_dialog_done, &state);
    g_main_loop_run(state.loop);
    g_main_loop_unref(state.loop);
    size_t written = 0;
    if (state.file) {
        char *path = g_file_get_path(state.file);
        if (path) {
            size_t len = strlen(path);
            written = len < buffer_len ? len : buffer_len;
            memcpy(buffer, path, written);
            g_free(path);
        }
        g_object_unref(state.file);
    }
    if (title) free(title);
    if (default_name) free(default_name);
    g_object_unref(dialog);
    return written;
}

typedef struct zero_native_alert_state {
    GMainLoop *loop;
    int response;
} zero_native_alert_state_t;

static void zero_native_alert_done(GObject *source, GAsyncResult *result, gpointer data) {
    zero_native_alert_state_t *state = data;
    GError *error = NULL;
    state->response = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(source), result, &error);
    if (error) {
        g_error_free(error);
        state->response = 0;
    }
    g_main_loop_quit(state->loop);
}

int zero_native_gtk_show_message_dialog(zero_native_gtk_host_t *host, const zero_native_gtk_message_dialog_opts_t *opts) {
    GtkAlertDialog *dialog = gtk_alert_dialog_new(NULL);
    char *title = zero_native_bytes_to_string(opts->title, opts->title_len);
    char *message = zero_native_bytes_to_string(opts->message, opts->message_len);
    char *informative = zero_native_bytes_to_string(opts->informative_text, opts->informative_text_len);
    char *primary = zero_native_bytes_to_string(opts->primary_button, opts->primary_button_len);
    char *secondary = zero_native_bytes_to_string(opts->secondary_button, opts->secondary_button_len);
    char *tertiary = zero_native_bytes_to_string(opts->tertiary_button, opts->tertiary_button_len);
    gtk_alert_dialog_set_message(dialog, title ? title : (message ? message : ""));
    if (informative || (title && message)) gtk_alert_dialog_set_detail(dialog, informative ? informative : message);
    const char *buttons[4] = { primary ? primary : "OK", NULL, NULL, NULL };
    if (secondary) buttons[1] = secondary;
    if (tertiary) buttons[2] = tertiary;
    gtk_alert_dialog_set_buttons(dialog, buttons);
    zero_native_alert_state_t state = { .loop = g_main_loop_new(NULL, FALSE), .response = 0 };
    if (state.loop) {
        gtk_alert_dialog_choose(dialog, zero_native_parent_window(host), NULL, zero_native_alert_done, &state);
        g_main_loop_run(state.loop);
        g_main_loop_unref(state.loop);
    }
    if (title) free(title);
    if (message) free(message);
    if (informative) free(informative);
    if (primary) free(primary);
    if (secondary) free(secondary);
    if (tertiary) free(tertiary);
    g_object_unref(dialog);
    if (state.response <= 0) return 0;
    if (state.response == 1) return 1;
    return 2;
}
