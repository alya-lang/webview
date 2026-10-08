// macOS backend for the Alya webview package: WKWebView.
//
// Pure C over the Objective-C runtime C API (same technique as the gui
// package's cocoa_window.c): no ObjC syntax, so this file needs no special
// compiler mode and links only against system frameworks (Cocoa + WebKit).
// The JS completion handler is a stack-block literal; WebKit copies it
// on dispatch and releases it when the callback completes.
//
// Threading: all WebKit work happens on the calling (main) thread while
// poll() spins the runloop non-blocking. No worker threads are created.

#include "webview.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

// --- Objective-C runtime C API (declared manually, no headers needed) ---

typedef struct objc_class *Class;
typedef struct objc_object *id;
typedef const struct objc_selector *SEL;
typedef signed char BOOL;
typedef void (*IMP)(void);
typedef struct objc_ivar *Ivar;

extern Class objc_getClass(const char *name);
extern SEL sel_registerName(const char *name);
extern id objc_msgSend(id self, SEL op, ...);
extern Class objc_allocateClassPair(Class superclass, const char *name,
                                    size_t extraBytes);
extern void objc_registerClassPair(Class cls);
extern BOOL class_addMethod(Class cls, SEL name, IMP imp,
                            const char *types);
extern BOOL class_addIvar(Class cls, const char *name, size_t size,
                          unsigned char alignment, const char *types);
extern Ivar class_getInstanceVariable(Class cls, const char *name);
extern id object_setIvar(id obj, Ivar ivar, id value);
extern id object_getIvar(id obj, Ivar ivar);
typedef struct objc_protocol Protocol;
extern Protocol *objc_getProtocol(const char *name);
extern BOOL class_addProtocol(Class cls, Protocol *protocol);
extern void *_NSConcreteStackBlock[32];
#if defined(__x86_64__)
extern double objc_msgSend_fpret(id self, SEL op, ...);
extern void objc_msgSend_stret(void *st, id self, SEL op, ...);
#endif

static double wv_msg_send_double(id target, SEL op) {
#if defined(__x86_64__)
    return objc_msgSend_fpret(target, op);
#else
    return ((double (*)(id, SEL))objc_msgSend)(target, op);
#endif
}

// Typed dispatch macros:
// On ARM64 (Apple Silicon), the calling convention for variadic functions differs
// from non-variadic functions: variadic arguments are placed on the stack instead of
// in registers x2-x7. Objective-C methods are non-variadic functions expecting arguments
// in registers. Calling objc_msgSend without casting to a matching non-variadic function
// pointer passes parameters on the stack, causing target methods to read garbage registers
// and crash with SIGSEGV. We cast objc_msgSend to the exact prototype for every dispatch.
#define wv_send0(ret, target, sel) \
    (((ret (*)(id, SEL))objc_msgSend)((id)(target), (sel)))
#define wv_send1(ret, target, sel, a1) \
    (((ret (*)(id, SEL, __typeof__(a1)))objc_msgSend)((id)(target), (sel), (a1)))
#define wv_send2(ret, target, sel, a1, a2) \
    (((ret (*)(id, SEL, __typeof__(a1), __typeof__(a2)))objc_msgSend)((id)(target), (sel), (a1), (a2)))
#define wv_send3(ret, target, sel, a1, a2, a3) \
    (((ret (*)(id, SEL, __typeof__(a1), __typeof__(a2), __typeof__(a3)))objc_msgSend)((id)(target), (sel), (a1), (a2), (a3)))
#define wv_send4(ret, target, sel, a1, a2, a3, a4) \
    (((ret (*)(id, SEL, __typeof__(a1), __typeof__(a2), __typeof__(a3), __typeof__(a4)))objc_msgSend)((id)(target), (sel), (a1), (a2), (a3), (a4)))
#define wv_send9(ret, target, sel, a1, a2, a3, a4, a5, a6, a7, a8, a9) \
    (((ret (*)(id, SEL, __typeof__(a1), __typeof__(a2), __typeof__(a3), __typeof__(a4), __typeof__(a5), __typeof__(a6), __typeof__(a7), __typeof__(a8), __typeof__(a9)))objc_msgSend)((id)(target), (sel), (a1), (a2), (a3), (a4), (a5), (a6), (a7), (a8), (a9)))
#define wv_send10(ret, target, sel, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10) \
    (((ret (*)(id, SEL, __typeof__(a1), __typeof__(a2), __typeof__(a3), __typeof__(a4), __typeof__(a5), __typeof__(a6), __typeof__(a7), __typeof__(a8), __typeof__(a9), __typeof__(a10)))objc_msgSend)((id)(target), (sel), (a1), (a2), (a3), (a4), (a5), (a6), (a7), (a8), (a9), (a10)))

typedef struct NSRect {
    double x;
    double y;
    double w;
    double h;
} NSRect;

typedef struct NSPoint {
    double x;
    double y;
} NSPoint;

typedef struct NSSize {
    double w;
    double h;
} NSSize;

#define WV_MAX_EVENTS 64

typedef struct {
    int kind;
    int w;
    int h;
    char url[ALYA_WEBVIEW_URL_CAP];
    char text[ALYA_WEBVIEW_TEXT_CAP];
} wv_event_t;

struct alya_webview {
    id win;      // NSWindow*
    id view;     // WKWebView*
    id delegate; // AlyaWebviewDelegate* (navigation + messages + close)
    int open;
    int ready;
    int width;
    int height;
    int eval_state;
    int eval_pending; // a completion block is still outstanding
    uint64_t eval_seq; // sequence counter for eval completions
    int allow_menu;   // native context menu policy (default 1)
    int block_keys;   // shortcut-blocking user script installed
    int is_fullscreen;
    unsigned long orig_mask; // window style at creation
    char url[ALYA_WEBVIEW_URL_CAP];
    char title[ALYA_WEBVIEW_TEXT_CAP];
    char message[ALYA_WEBVIEW_TEXT_CAP];
    char eval_result[ALYA_WEBVIEW_TEXT_CAP];
    int head;
    int tail;
    wv_event_t queue[WV_MAX_EVENTS];
};

/* --- tiny helpers --- */

static void wv_copy(char *dst, size_t cap, const char *src) {
    size_t n;
    if (cap == 0) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    n = strlen(src);
    if (n > cap - 1) {
        n = cap - 1;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static SEL wv_sel(const char *name) {
    static struct {
        const char *name;
        SEL sel;
    } cache[96];
    static int used = 0;
    int i;
    for (i = 0; i < used; i++) {
        if (strcmp(cache[i].name, name) == 0) {
            return cache[i].sel;
        }
    }
    if (used < 96) {
        cache[used].name = name;
        cache[used].sel = sel_registerName(name);
        used++;
    } else {
        return sel_registerName(name);
    }
    return cache[used - 1].sel;
}

static id wv_nsstr(const char *s) {
    if (s == NULL) {
        s = "";
    }
    return wv_send1(id, (id)objc_getClass("NSString"),
                    wv_sel("stringWithUTF8String:"), s);
}

static const char *wv_cstr(id nsstr) {
    if (nsstr == NULL) {
        return "";
    }
    return wv_send0(const char *, nsstr, wv_sel("UTF8String"));
}

/* --- autorelease pool helpers (no ObjC syntax available) --- */

static id wv_pool_push(void) {
    id pool = wv_send0(id, (id)objc_getClass("NSAutoreleasePool"),
                       wv_sel("alloc"));
    return wv_send0(id, pool, wv_sel("init"));
}

static void wv_pool_pop(id pool) {
    if (pool != NULL) {
        wv_send0(void, pool, wv_sel("drain"));
    }
}

static void wv_push(alya_webview_t *w, int kind) {
    int next;
    int prev;
    if (w == NULL) {
        return;
    }
    // Deduplicate consecutive identical NAV_DONE events for the same URL
    if (kind == ALYA_WEBVIEW_EVENT_NAV_DONE && w->head != w->tail) {
        prev = (w->tail - 1 + WV_MAX_EVENTS) % WV_MAX_EVENTS;
        if (w->queue[prev].kind == ALYA_WEBVIEW_EVENT_NAV_DONE &&
            strcmp(w->queue[prev].url, w->url) == 0) {
            return;
        }
    }
    next = (w->tail + 1) % WV_MAX_EVENTS;
    if (next == w->head) {
        return; // full: drop newest
    }
    w->queue[w->tail].kind = kind;
    w->queue[w->tail].w = w->width;
    w->queue[w->tail].h = w->height;
    wv_copy(w->queue[w->tail].url, sizeof(w->queue[w->tail].url), w->url);
    if (kind == ALYA_WEBVIEW_EVENT_MESSAGE) {
        wv_copy(w->queue[w->tail].text, sizeof(w->queue[w->tail].text),
                w->message);
    } else if (kind == ALYA_WEBVIEW_EVENT_TITLE) {
        wv_copy(w->queue[w->tail].text, sizeof(w->queue[w->tail].text),
                w->title);
    } else {
        w->queue[w->tail].text[0] = '\0';
    }
    w->tail = next;
}

static alya_webview_t *wv_ctx_of(id delegate) {
    Ivar iv;
    if (delegate == NULL) {
        return NULL;
    }
    iv = class_getInstanceVariable(objc_getClass("AlyaWebviewDelegate"),
                                   "wvCtx");
    if (iv == NULL) {
        return NULL;
    }
    return (alya_webview_t *)(void *)object_getIvar(delegate, iv);
}

static void wv_sync_url(alya_webview_t *w) {
    id view;
    id url;
    id abs;
    if (w == NULL || w->view == NULL) {
        return;
    }
    view = w->view;
    url = wv_send0(id, view, wv_sel("URL"));
    if (url == NULL) {
        return;
    }
    abs = wv_send0(id, url, wv_sel("absoluteString"));
    wv_copy(w->url, sizeof(w->url), wv_cstr(abs));
}

static void wv_sync_title(alya_webview_t *w) {
    id t;
    if (w == NULL || w->view == NULL) {
        return;
    }
    t = wv_send0(id, w->view, wv_sel("title"));
    wv_copy(w->title, sizeof(w->title), wv_cstr(t));
}

/* --- delegate callbacks (C functions installed as ObjC methods) --- */

static void wv_did_start(id self, SEL cmd, id webview, id navigation) {
    alya_webview_t *w = wv_ctx_of(self);
    (void)cmd;
    (void)navigation;
    if (w == NULL) {
        return;
    }
    (void)webview;
    wv_sync_url(w);
    wv_push(w, ALYA_WEBVIEW_EVENT_NAV_START);
}

static void wv_did_finish(id self, SEL cmd, id webview, id navigation) {
    alya_webview_t *w = wv_ctx_of(self);
    (void)cmd;
    (void)navigation;
    if (w == NULL) {
        return;
    }
    (void)webview;
    wv_sync_url(w);
    wv_push(w, ALYA_WEBVIEW_EVENT_NAV_DONE);
}

static void wv_did_fail(id self, SEL cmd, id webview, id navigation,
                        id error) {
    alya_webview_t *w = wv_ctx_of(self);
    (void)cmd;
    (void)navigation;
    (void)error;
    if (w == NULL) {
        return;
    }
    (void)webview;
    wv_sync_url(w);
    wv_push(w, ALYA_WEBVIEW_EVENT_NAV_FAILED);
}

static void wv_did_message(id self, SEL cmd, id controller, id message) {
    alya_webview_t *w = wv_ctx_of(self);
    id body;
    id desc;
    (void)cmd;
    (void)controller;
    if (w == NULL || message == NULL) {
        return;
    }
    body = wv_send0(id, message, wv_sel("body"));
    if (body == NULL) {
        return;
    }
    desc = wv_send0(id, body, wv_sel("description"));
    wv_copy(w->message, sizeof(w->message), wv_cstr(desc));
    wv_push(w, ALYA_WEBVIEW_EVENT_MESSAGE);
}

static void wv_did_observe(id self, SEL cmd, id obj, id path, id change,
                           void *ctx) {
    alya_webview_t *w = wv_ctx_of(self);
    (void)cmd;
    (void)obj;
    (void)path;
    (void)change;
    (void)ctx;
    if (w == NULL) {
        return;
    }
    wv_sync_title(w);
    wv_push(w, ALYA_WEBVIEW_EVENT_TITLE);
}

static void wv_will_close(id self, SEL cmd, id notification) {
    alya_webview_t *w = wv_ctx_of(self);
    (void)cmd;
    (void)notification;
    if (w == NULL) {
        return;
    }
    w->open = 0;
    wv_push(w, ALYA_WEBVIEW_EVENT_CLOSE);
}

static Class wv_delegate_class(void) {
    static Class cls = NULL;
    if (cls == NULL) {
        Class nsobj = objc_getClass("NSObject");
        cls = objc_allocateClassPair(nsobj, "AlyaWebviewDelegate", 0);
        class_addIvar(cls, "wvCtx", sizeof(void *), 3, "^v");
        Protocol *protoNav = objc_getProtocol("WKNavigationDelegate");
        if (protoNav != NULL) {
            class_addProtocol(cls, protoNav);
        }
        Protocol *protoMsg = objc_getProtocol("WKScriptMessageHandler");
        if (protoMsg != NULL) {
            class_addProtocol(cls, protoMsg);
        }
        class_addMethod(cls,
                        sel_registerName("webView:didStartProvisionalNavigation:"),
                        (IMP)wv_did_start, "v@:@@");
        class_addMethod(cls,
                        sel_registerName("webView:didFinishNavigation:"),
                        (IMP)wv_did_finish, "v@:@@");
        class_addMethod(cls,
                        sel_registerName("webView:didFailNavigation:withError:"),
                        (IMP)wv_did_fail, "v@:@@@");
        class_addMethod(cls,
                        sel_registerName("webView:didFailProvisionalNavigation:withError:"),
                        (IMP)wv_did_fail, "v@:@@@");
        class_addMethod(cls,
                        sel_registerName("userContentController:didReceiveScriptMessage:"),
                        (IMP)wv_did_message, "v@:@@");
        class_addMethod(cls,
                        sel_registerName("observeValueForKeyPath:ofObject:change:context:"),
                        (IMP)wv_did_observe, "v@:@@@@^v");
        class_addMethod(cls, sel_registerName("windowWillClose:"),
                        (IMP)wv_will_close, "v@:@");
        objc_registerClassPair(cls);
    }
    return cls;
}

/* --- JS completion block (stack literal; WebKit copies to heap and releases) --- */

typedef struct wv_blk_desc {
    unsigned long reserved;
    unsigned long size;
} wv_blk_desc_t;

typedef struct wv_block {
    void *isa;
    int flags;
    int reserved;
    void (*invoke)(void *blk, id result, id error);
    wv_blk_desc_t *desc;
    alya_webview_t *w;
    uint64_t seq;
} wv_block_t;

static wv_blk_desc_t wv_blk_desc = {0, sizeof(wv_block_t)};

static void wv_js_invoke(void *blk, id result, id error) {
    id pool = wv_pool_push();
    wv_block_t *b = (wv_block_t *)blk;
    alya_webview_t *w = (b != NULL) ? b->w : NULL;
    if (w != NULL) {
        // Discard stale completions from earlier fire-and-forget evals
        if (b->seq != w->eval_seq) {
            wv_pool_pop(pool);
            return;
        }
        if (error == NULL) {
            if (result == NULL) {
                w->eval_result[0] = '\0';
            } else {
                Class numCls = objc_getClass("NSNumber");
                if (numCls != NULL &&
                    wv_send1(BOOL, result, wv_sel("isKindOfClass:"), (id)numCls)) {
                    const char *type =
                        wv_send0(const char *, result, wv_sel("objCType"));
                    if (type != NULL && strcmp(type, "c") == 0) {
                        BOOL bval = wv_send0(BOOL, result, wv_sel("boolValue"));
                        wv_copy(w->eval_result, sizeof(w->eval_result),
                                bval ? "true" : "false");
                    } else {
                        id desc = wv_send0(id, result, wv_sel("description"));
                        wv_copy(w->eval_result, sizeof(w->eval_result),
                                wv_cstr(desc));
                    }
                } else {
                    id desc = wv_send0(id, result, wv_sel("description"));
                    wv_copy(w->eval_result, sizeof(w->eval_result),
                            wv_cstr(desc));
                }
            }
            w->eval_state = ALYA_WEBVIEW_EVAL_READY;
        } else {
            w->eval_result[0] = '\0';
            w->eval_state = ALYA_WEBVIEW_EVAL_ERROR;
        }
        w->eval_pending = 0;
    }
    wv_pool_pop(pool);
}

static int wv_fire_js(alya_webview_t *w, const char *js) {
    wv_block_t b;
    id code;
    if (w == NULL || w->view == NULL) {
        return 0;
    }
    if (js == NULL || js[0] == '\0') {
        return 0;
    }
    memset(&b, 0, sizeof(b));
    b.isa = _NSConcreteStackBlock;
    b.flags = 0;
    b.reserved = 0;
    b.invoke = wv_js_invoke;
    b.desc = &wv_blk_desc;
    b.w = w;
    b.seq = ++w->eval_seq;
    code = wv_nsstr(js);
    w->eval_state = ALYA_WEBVIEW_EVAL_PENDING;
    w->eval_result[0] = '\0';
    w->eval_pending = 1;
    wv_send2(void, w->view, wv_sel("evaluateJavaScript:completionHandler:"),
             code, (id)&b);
    return 1;
}

/* Local folder hosting (alya://host/path). The scheme handler is
 * installed on every configuration at creation; serve/clear mutate a
 * process-wide host table consulted per request. Only regular files
 * are served (GET); ".." traversal and misses yield 404. */

#define WV_MAX_MAPS 8
#define WV_MAX_FILE (8 * 1024 * 1024)

static struct {
    char host[128];
    char folder[1024];
    int used;
} wv_maps[WV_MAX_MAPS];

int alya_webview_serve_folder(const char *host, const char *folder) {
    int i;
    int free_slot = -1;
    if (host == NULL || host[0] == '\0' || folder == NULL ||
        folder[0] == '\0') {
        return 0;
    }
    for (i = 0; i < WV_MAX_MAPS; i++) {
        if (wv_maps[i].used && strcmp(wv_maps[i].host, host) == 0) {
            free_slot = i;
            break;
        }
        if (!wv_maps[i].used && free_slot < 0) {
            free_slot = i;
        }
    }
    if (free_slot < 0) {
        return 0;
    }
    strncpy(wv_maps[free_slot].host, host,
            sizeof(wv_maps[free_slot].host) - 1);
    wv_maps[free_slot].host[sizeof(wv_maps[free_slot].host) - 1] = '\0';
    strncpy(wv_maps[free_slot].folder, folder,
            sizeof(wv_maps[free_slot].folder) - 1);
    wv_maps[free_slot].folder[sizeof(wv_maps[free_slot].folder) - 1] = '\0';
    wv_maps[free_slot].used = 1;
    return 1;
}

int alya_webview_clear_mapping(const char *host) {
    int i;
    if (host == NULL || host[0] == '\0') {
        for (i = 0; i < WV_MAX_MAPS; i++) {
            wv_maps[i].used = 0;
        }
        return 1;
    }
    for (i = 0; i < WV_MAX_MAPS; i++) {
        if (wv_maps[i].used && strcmp(wv_maps[i].host, host) == 0) {
            wv_maps[i].used = 0;
            return 1;
        }
    }
    return 1;
}

static const char *wv_mime_for(const char *path) {
    static const struct {
        const char *ext;
        const char *mime;
    } map[] = {{".html", "text/html"},
               {".htm", "text/html"},
               {".js", "application/javascript"},
               {".css", "text/css"},
               {".json", "application/json"},
               {".png", "image/png"},
               {".jpg", "image/jpeg"},
               {".jpeg", "image/jpeg"},
               {".gif", "image/gif"},
               {".svg", "image/svg+xml"},
               {".ico", "image/x-icon"},
               {".txt", "text/plain"},
               {".alya", "text/plain"},
               {".wasm", "application/wasm"},
               {".mp4", "video/mp4"},
               {".webm", "video/webm"},
               {".mp3", "audio/mpeg"},
               {".woff2", "font/woff2"},
               {NULL, NULL}};
    const char *dot = strrchr(path, '.');
    int i;
    if (dot == NULL) {
        return "text/plain";
    }
    for (i = 0; map[i].ext != NULL; i++) {
        if (strcmp(dot, map[i].ext) == 0) {
            return map[i].mime;
        }
    }
    return "text/plain";
}

static void wv_scheme_fail(id task) {
    id pool = wv_pool_push();
    id err = wv_send3(id, (id)objc_getClass("NSError"),
                      wv_sel("errorWithDomain:code:userInfo:"),
                      wv_nsstr("alya"), (long)404, NULL);
    wv_send1(void, task, wv_sel("didFailWithError:"), err);
    wv_pool_pop(pool);
}

static void wv_scheme_start(id self, SEL cmd, id webview, id task) {
    id pool;
    id req;
    id url;
    const char *abs;
    const char *p;
    const char *slash;
    char host[128];
    char full[2048];
    size_t hlen;
    int i;
    FILE *f;
    long size;
    char *data;
    (void)self;
    (void)cmd;
    if (task == NULL) {
        return;
    }
    pool = wv_pool_push();
    req = wv_send0(id, task, wv_sel("request"));
    url = req != NULL
              ? wv_send0(id, req, wv_sel("URL"))
              : NULL;
    abs = url != NULL
              ? wv_cstr(wv_send0(id, url, wv_sel("absoluteString")))
              : "";
    // Expect alya://host/path
    p = strstr(abs, "://");
    if (p == NULL) {
        wv_pool_pop(pool);
        wv_scheme_fail(task);
        return;
    }
    p += 3;
    slash = strchr(p, '/');
    hlen = slash != NULL ? (size_t)(slash - p) : strlen(p);
    if (hlen == 0 || hlen > sizeof(host) - 1) {
        wv_pool_pop(pool);
        wv_scheme_fail(task);
        return;
    }
    memcpy(host, p, hlen);
    host[hlen] = '\0';
    for (i = 0; i < WV_MAX_MAPS; i++) {
        if (wv_maps[i].used && strcmp(wv_maps[i].host, host) == 0) {
            break;
        }
    }
    if (i >= WV_MAX_MAPS) {
        wv_pool_pop(pool);
        wv_scheme_fail(task);
        return;
    }
    if (slash == NULL || slash[1] == '\0') {
        snprintf(full, sizeof(full), "%s/index.html", wv_maps[i].folder);
    } else {
        const char *rel = (slash[0] == '/') ? slash + 1 : slash;
        if (strstr(rel, "..") != NULL) {
            wv_pool_pop(pool);
            wv_scheme_fail(task);
            return;
        }
        snprintf(full, sizeof(full), "%s/%s", wv_maps[i].folder, rel);
    }
    f = fopen(full, "rb");
    if (f == NULL && full[0] == '.' && full[1] == '/') {
        f = fopen(full + 2, "rb");
    }
    if (f == NULL) {
        wv_pool_pop(pool);
        wv_scheme_fail(task);
        return;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0 || size > WV_MAX_FILE) {
        fclose(f);
        wv_pool_pop(pool);
        wv_scheme_fail(task);
        return;
    }
    data = (char *)malloc(size > 0 ? (size_t)size : 1);
    if (data == NULL) {
        fclose(f);
        wv_pool_pop(pool);
        wv_scheme_fail(task);
        return;
    }
    if (size > 0 && fread(data, 1, (size_t)size, f) != (size_t)size) {
        free(data);
        fclose(f);
        wv_pool_pop(pool);
        wv_scheme_fail(task);
        return;
    }
    fclose(f);
    {
        id dataObj = wv_send2(id, (id)objc_getClass("NSData"),
                              wv_sel("dataWithBytes:length:"), data,
                              (unsigned long)(size > 0 ? size : 0));
        id resp;
        free(data);
        resp = wv_send0(id, (id)objc_getClass("NSURLResponse"),
                        wv_sel("alloc"));
        resp = wv_send4(id, resp,
                        wv_sel("initWithURL:MIMEType:expectedContentLength:textEncodingName:"),
                        url, wv_nsstr(wv_mime_for(full)), (long)size,
                        wv_nsstr("utf-8"));
        wv_send1(void, task, wv_sel("didReceiveResponse:"), resp);
        wv_send1(void, task, wv_sel("didReceiveData:"), dataObj);
        wv_send0(void, task, wv_sel("didFinish"));
        wv_send0(void, resp, wv_sel("release"));
    }
    if (webview != NULL) {
        id del = wv_send0(id, webview, wv_sel("navigationDelegate"));
        alya_webview_t *w = wv_ctx_of(del);
        if (w != NULL) {
            wv_copy(w->url, sizeof(w->url), abs);
            wv_push(w, ALYA_WEBVIEW_EVENT_NAV_DONE);
        }
    }
    wv_pool_pop(pool);
}

static void wv_scheme_stop(id self, SEL cmd, id webview, id task) {
    // Synchronous serving: nothing to cancel.
    (void)self;
    (void)cmd;
    (void)webview;
    (void)task;
}

static Class wv_scheme_class(void) {
    static Class cls = NULL;
    if (cls == NULL) {
        Class nsobj = objc_getClass("NSObject");
        Protocol *proto;
        cls = objc_allocateClassPair(nsobj, "AlyaWebSchemeHandler", 0);
        proto = objc_getProtocol("WKURLSchemeHandler");
        if (proto != NULL) {
            class_addProtocol(cls, proto);
        }
        class_addMethod(cls,
                        sel_registerName("webView:startURLSchemeTask:"),
                        (IMP)wv_scheme_start, "v@:@@");
        class_addMethod(cls,
                        sel_registerName("webView:stopURLSchemeTask:"),
                        (IMP)wv_scheme_stop, "v@:@@");
        objc_registerClassPair(cls);
    }
    return cls;
}

static id wv_get_scheme_handler(void) {
    static id handler = NULL;
    if (handler == NULL) {
        id sh = wv_send0(id, (id)wv_scheme_class(), wv_sel("alloc"));
        handler = wv_send0(id, sh, wv_sel("init"));
    }
    return handler;
}


/* ---------------- public contract ---------------- */

int alya_webview_backend_id(void) {
    return 2;
}

const char *alya_webview_backend_name(void) {
    return "macos";
}

const char *alya_webview_engine_version(void) {
    // WebKit framework bundle version (always present with WKWebView).
    static char cached[64];
    static int probed = 0;
    id pool;
    id bundle;
    id ver;
    Class cls;
    if (probed) {
        return cached;
    }
    probed = 1;
    cached[0] = '\0';
    cls = objc_getClass("NSBundle");
    if (cls == NULL) {
        return "";
    }
    pool = wv_pool_push();
    bundle = wv_send1(id, (id)cls,
                      wv_sel("bundleWithIdentifier:"),
                      wv_nsstr("com.apple.WebKit"));
    if (bundle != NULL) {
        ver = wv_send1(id, bundle,
                       wv_sel("objectForInfoDictionaryKey:"),
                       wv_nsstr("CFBundleShortVersionString"));
        if (ver == NULL) {
            ver = wv_send1(id, bundle,
                           wv_sel("objectForInfoDictionaryKey:"),
                           wv_nsstr("CFBundleVersion"));
        }
        if (ver != NULL) {
            const char *cs = wv_cstr(ver);
            if (cs != NULL && cs[0] != '\0') {
                wv_copy(cached, sizeof(cached), cs);
            }
        }
    }
    if (cached[0] == '\0') {
        if (objc_getClass("WKWebView") != NULL) {
            wv_copy(cached, sizeof(cached), "WebKit");
        }
    }
    wv_pool_pop(pool);
    return cached;
}

int alya_webview_open_external(const char *url) {
    id pool;
    id ws;
    id nsurl;
    long ok = 0;
    if (url == NULL || url[0] == '\0') {
        return 0;
    }
    pool = wv_pool_push();
    ws = wv_send0(id, (id)objc_getClass("NSWorkspace"),
                  wv_sel("sharedWorkspace"));
    if (ws != NULL) {
        nsurl = wv_send1(id, (id)objc_getClass("NSURL"),
                         wv_sel("URLWithString:"), wv_nsstr(url));
        if (nsurl != NULL) {
            ok = wv_send1(BOOL, ws, wv_sel("openURL:"), nsurl);
        }
    }
    wv_pool_pop(pool);
    return ok ? 1 : 0;
}

/* Launch configuration record (macOS applies neither: WKWebView has
 * no custom-path or switch channel; private windows use a
 * non-persistent store). Getters report what was configured. */
static char wv_g_data_dir[1024];
static char wv_g_extra_args[2048];

void alya_webview_set_data_dir(const char *path) {
    if (path == NULL || path[0] == '\0') {
        wv_g_data_dir[0] = '\0';
        return;
    }
    strncpy(wv_g_data_dir, path, sizeof(wv_g_data_dir) - 1);
    wv_g_data_dir[sizeof(wv_g_data_dir) - 1] = '\0';
}

const char *alya_webview_get_data_dir(void) {
    return wv_g_data_dir;
}

void alya_webview_set_extra_args(const char *args) {
    if (args == NULL || args[0] == '\0') {
        wv_g_extra_args[0] = '\0';
        return;
    }
    strncpy(wv_g_extra_args, args, sizeof(wv_g_extra_args) - 1);
    wv_g_extra_args[sizeof(wv_g_extra_args) - 1] = '\0';
}

const char *alya_webview_get_extra_args(void) {
    return wv_g_extra_args;
}

const char *alya_webview_profile_path(alya_webview_t *w) {
    // Default store and private windows have no filesystem path.
    (void)w;
    return "";
}

static alya_webview_t *wv_create_inner(const char *title, int width,
                                               int height, int priv);

alya_webview_t *alya_webview_create(const char *title, int width,
                                    int height) {
    return wv_create_inner(title, width, height, 0);
}

alya_webview_t *alya_webview_create_private(const char *title, int width,
                                            int height) {
    return wv_create_inner(title, width, height, 1);
}

static alya_webview_t *wv_create_inner(const char *title, int width,
                                       int height, int priv) {
    alya_webview_t *w;
    id pool;
    id app;
    id win;
    id config;
    id ucc;
    id view;
    id del;
    id content;
    Ivar iv;
    NSRect rect;

    if (width <= 0) {
        width = 800;
    }
    if (height <= 0) {
        height = 600;
    }
    if (title == NULL || title[0] == '\0') {
        title = "Alya";
    }
    // WKWebView must exist on this system.
    if (objc_getClass("WKWebView") == NULL) {
        return NULL;
    }

    w = (alya_webview_t *)calloc(1, sizeof(*w));
    if (w == NULL) {
        return NULL;
    }
    w->open = 1;
    w->allow_menu = 1;
    w->width = width;
    w->height = height;
    wv_copy(w->title, sizeof(w->title), title);

    pool = wv_pool_push();
    app = wv_send0(id, (id)objc_getClass("NSApplication"),
                   wv_sel("sharedApplication"));
    if (app == NULL) {
        wv_pool_pop(pool);
        free(w);
        return NULL;
    }
    wv_send1(void, app, wv_sel("setActivationPolicy:"), (long)0);
    wv_send0(void, app, wv_sel("finishLaunching"));

    rect.x = 100;
    rect.y = 100;
    rect.w = (double)width;
    rect.h = (double)height;
    win = wv_send0(id, (id)objc_getClass("NSWindow"), wv_sel("alloc"));
    win = wv_send4(id, win, wv_sel("initWithContentRect:styleMask:backing:defer:"),
                   rect, (unsigned long)15, (unsigned long)2, (BOOL)0);
    if (win == NULL) {
        wv_pool_pop(pool);
        free(w);
        return NULL;
    }
    wv_send1(void, win, wv_sel("setTitle:"), wv_nsstr(title));
    wv_send1(void, win, wv_sel("setReleasedWhenClosed:"), (BOOL)0);
    w->orig_mask = wv_send0(unsigned long, win, wv_sel("styleMask"));

    config = wv_send0(id, (id)objc_getClass("WKWebViewConfiguration"),
                      wv_sel("alloc"));
    config = wv_send0(id, config, wv_sel("init"));
    if (priv) {
        id store = wv_send0(id, (id)objc_getClass("WKWebsiteDataStore"),
                            wv_sel("nonPersistentDataStore"));
        if (store != NULL) {
            wv_send1(void, config, wv_sel("setWebsiteDataStore:"), store);
        }
    }
    // App-scheme handler (consulting the serve_folder table per request).
    {
        id sh = wv_get_scheme_handler();
        if (sh != NULL) {
            wv_send2(void, config, wv_sel("setURLSchemeHandler:forURLScheme:"),
                     sh, wv_nsstr("alya"));
        }
    }
    ucc = wv_send0(id, config, wv_sel("userContentController"));

    del = wv_send0(id, (id)wv_delegate_class(), wv_sel("alloc"));
    del = wv_send0(id, del, wv_sel("init"));
    iv = class_getInstanceVariable(wv_delegate_class(), "wvCtx");
    object_setIvar(del, iv, (id)(void *)w);

    wv_send2(void, ucc, wv_sel("addScriptMessageHandler:name:"), del,
             wv_nsstr("alya"));

    content = wv_send0(id, win, wv_sel("contentView"));
    {
        // Start at the requested size (kept in sync on resize).
        NSRect fr;
        fr.x = 0;
        fr.y = 0;
        fr.w = (double)width;
        fr.h = (double)height;
        view = wv_send0(id, (id)objc_getClass("WKWebView"), wv_sel("alloc"));
        view = wv_send2(id, view, wv_sel("initWithFrame:configuration:"),
                        fr, config);
    }
    if (view == NULL) {
        wv_send0(void, del, wv_sel("release"));
        wv_send0(void, config, wv_sel("release"));
        wv_send0(void, win, wv_sel("release"));
        wv_pool_pop(pool);
        free(w);
        return NULL;
    }
    wv_send1(void, view, wv_sel("setNavigationDelegate:"), del);
    wv_send1(void, view, wv_sel("setAutoresizingMask:"),
             (unsigned long)(2 | 16));
    wv_send1(void, content, wv_sel("addSubview:"), view);
    wv_send4(void, view, wv_sel("addObserver:forKeyPath:options:context:"),
             del, wv_nsstr("title"), (unsigned long)0, NULL);
    wv_send1(void, win, wv_sel("setDelegate:"), del);

    w->win = win;
    w->view = view;
    w->delegate = del;
    w->ready = 1;
    wv_pool_pop(pool);
    return w;
}

void alya_webview_destroy(alya_webview_t *w) {
    id pool;
    if (w == NULL) {
        return;
    }
    pool = wv_pool_push();
    if (w->view != NULL) {
        wv_send2(void, w->view,
                 wv_sel("removeObserver:forKeyPath:"), w->delegate,
                 wv_nsstr("title"));
        wv_send1(void, w->view, wv_sel("setNavigationDelegate:"), NULL);
        wv_send0(void, w->view, wv_sel("removeFromSuperview"));
        wv_send0(void, w->view, wv_sel("release"));
        w->view = NULL;
    }
    if (w->win != NULL) {
        wv_send1(void, w->win, wv_sel("setDelegate:"), NULL);
        wv_send0(void, w->win, wv_sel("close"));
        wv_send0(void, w->win, wv_sel("release"));
        w->win = NULL;
    }
    if (w->delegate != NULL) {
        wv_send0(void, w->delegate, wv_sel("release"));
        w->delegate = NULL;
    }
    w->ready = 0;
    w->open = 0;
    wv_pool_pop(pool);
    free(w);
}

void alya_webview_show(alya_webview_t *w) {
    id pool;
    if (w == NULL || w->win == NULL) {
        return;
    }
    pool = wv_pool_push();
    wv_send1(void, w->win, wv_sel("makeKeyAndOrderFront:"), NULL);
    {
        id app = wv_send0(id, (id)objc_getClass("NSApplication"),
                          wv_sel("sharedApplication"));
        wv_send1(void, app, wv_sel("activateIgnoringOtherApps:"), (BOOL)1);
    }
    wv_pool_pop(pool);
}

void alya_webview_hide(alya_webview_t *w) {
    id pool;
    if (w == NULL || w->win == NULL) {
        return;
    }
    pool = wv_pool_push();
    wv_send1(void, w->win, wv_sel("orderOut:"), NULL);
    wv_pool_pop(pool);
}

int alya_webview_is_open(alya_webview_t *w) {
    if (w == NULL) {
        return 0;
    }
    return w->open && w->win != NULL;
}

int alya_webview_is_ready(alya_webview_t *w) {
    if (w == NULL) {
        return 0;
    }
    return w->ready && w->view != NULL;
}

void alya_webview_request_close(alya_webview_t *w) {
    id pool;
    if (w == NULL) {
        return;
    }
    w->open = 0;
    wv_push(w, ALYA_WEBVIEW_EVENT_CLOSE);
    if (w->win != NULL) {
        pool = wv_pool_push();
        wv_send0(void, w->win, wv_sel("close"));
        wv_pool_pop(pool);
    }
}

void alya_webview_set_title(alya_webview_t *w, const char *title) {
    id pool;
    if (w == NULL || w->win == NULL) {
        return;
    }
    if (title == NULL) {
        title = "";
    }
    wv_copy(w->title, sizeof(w->title), title);
    pool = wv_pool_push();
    wv_send1(void, w->win, wv_sel("setTitle:"), wv_nsstr(title));
    wv_pool_pop(pool);
}

void alya_webview_set_size(alya_webview_t *w, int width, int height) {
    id pool;
    NSSize size;
    if (w == NULL || w->win == NULL) {
        return;
    }
    if (width <= 0 || height <= 0) {
        return;
    }
    w->width = width;
    w->height = height;
    size.w = (double)width;
    size.h = (double)height;
    pool = wv_pool_push();
    wv_send1(void, w->win, wv_sel("setContentSize:"), size);
    wv_pool_pop(pool);
    wv_push(w, ALYA_WEBVIEW_EVENT_RESIZE);
}

static int wv_load_request(alya_webview_t *w, const char *url) {
    id pool;
    id nsurl;
    id req;
    if (w == NULL || w->view == NULL) {
        return 0;
    }
    if (url == NULL || url[0] == '\0') {
        return 0;
    }
    pool = wv_pool_push();
    nsurl = wv_send1(id, (id)objc_getClass("NSURL"),
                     wv_sel("URLWithString:"), wv_nsstr(url));
    if (nsurl == NULL) {
        wv_pool_pop(pool);
        return 0;
    }
    req = wv_send1(id, (id)objc_getClass("NSURLRequest"),
                   wv_sel("requestWithURL:"), nsurl);
    wv_send1(void, w->view, wv_sel("loadRequest:"), req);
    wv_pool_pop(pool);
    return 1;
}

int alya_webview_navigate(alya_webview_t *w, const char *url) {
    if (w == NULL || w->view == NULL) {
        return 0;
    }
    if (url == NULL || url[0] == '\0') {
        return 0;
    }
    wv_copy(w->url, sizeof(w->url), url);
    wv_push(w, ALYA_WEBVIEW_EVENT_NAV_START);
    return wv_load_request(w, url);
}

int alya_webview_load_html(alya_webview_t *w, const char *html) {
    id pool;
    if (w == NULL || w->view == NULL) {
        return 0;
    }
    if (html == NULL) {
        html = "";
    }
    wv_copy(w->url, sizeof(w->url), "about:blank");
    wv_push(w, ALYA_WEBVIEW_EVENT_NAV_START);
    pool = wv_pool_push();
    wv_send2(void, w->view, wv_sel("loadHTMLString:baseURL:"), wv_nsstr(html),
             NULL);
    wv_pool_pop(pool);
    return 1;
}

int alya_webview_reload(alya_webview_t *w) {
    id pool;
    if (w == NULL || w->view == NULL) {
        return 0;
    }
    pool = wv_pool_push();
    wv_send0(void, w->view, wv_sel("reload"));
    wv_pool_pop(pool);
    return 1;
}

int alya_webview_go_back(alya_webview_t *w) {
    id pool;
    if (w == NULL || w->view == NULL) {
        return 0;
    }
    pool = wv_pool_push();
    wv_send0(void, w->view, wv_sel("goBack"));
    wv_pool_pop(pool);
    return 1;
}

int alya_webview_go_forward(alya_webview_t *w) {
    id pool;
    if (w == NULL || w->view == NULL) {
        return 0;
    }
    pool = wv_pool_push();
    wv_send0(void, w->view, wv_sel("goForward"));
    wv_pool_pop(pool);
    return 1;
}

int alya_webview_can_back(alya_webview_t *w) {
    id pool;
    BOOL v = 0;
    if (w == NULL || w->view == NULL) {
        return 0;
    }
    pool = wv_pool_push();
    v = wv_send0(BOOL, w->view, wv_sel("canGoBack"));
    wv_pool_pop(pool);
    return v ? 1 : 0;
}

int alya_webview_can_forward(alya_webview_t *w) {
    id pool;
    BOOL v = 0;
    if (w == NULL || w->view == NULL) {
        return 0;
    }
    pool = wv_pool_push();
    v = wv_send0(BOOL, w->view, wv_sel("canGoForward"));
    wv_pool_pop(pool);
    return v ? 1 : 0;
}

int alya_webview_eval(alya_webview_t *w, const char *js) {
    id pool;
    int ok;
    if (w == NULL || w->view == NULL) {
        return 0;
    }
    if (js == NULL || js[0] == '\0') {
        return 0;
    }
    pool = wv_pool_push();
    ok = wv_fire_js(w, js);
    wv_pool_pop(pool);
    return ok;
}

int alya_webview_eval_state(alya_webview_t *w) {
    if (w == NULL) {
        return ALYA_WEBVIEW_EVAL_ERROR;
    }
    return w->eval_state;
}

int alya_webview_post_message(alya_webview_t *w, const char *json) {
    // Unified channel: deliver through window.postMessage, which pages
    // observe with window.addEventListener("message").
    char buf[ALYA_WEBVIEW_TEXT_CAP + 64];
    size_t n;
    if (w == NULL || w->view == NULL) {
        return 0;
    }
    if (json == NULL) {
        json = "null";
    }
    n = strlen(json);
    if (n > ALYA_WEBVIEW_TEXT_CAP - 1) {
        n = ALYA_WEBVIEW_TEXT_CAP - 1;
    }
    memcpy(buf, "window.postMessage(", 20);
    memcpy(buf + 20, json, n);
    memcpy(buf + 20 + n, ",\"*\")", 6);
    buf[20 + n + 5] = '\0';
    {
        id pool = wv_pool_push();
        // Fire-and-forget: no completion block needed for delivery.
        wv_send2(void, w->view,
                 wv_sel("evaluateJavaScript:completionHandler:"),
                 wv_nsstr(buf), NULL);
        wv_pool_pop(pool);
    }
    return 1;
}

int alya_webview_set_devtools(alya_webview_t *w, int enabled) {
    id pool;
    id config;
    id prefs;
    SEL sel_ins;
    SEL sel_dev;
    if (w == NULL || w->view == NULL) {
        return 0;
    }
    pool = wv_pool_push();
    // Modern WebKit (macOS 13.3+): WKWebView.isInspectable
    sel_ins = wv_sel("setInspectable:");
    if (wv_send1(BOOL, w->view, wv_sel("respondsToSelector:"), sel_ins)) {
        wv_send1(void, w->view, sel_ins, (BOOL)(enabled ? 1 : 0));
    }
    // Older WebKit: WKPreferences._setDeveloperExtrasEnabled:
    config = wv_send0(id, w->view, wv_sel("configuration"));
    if (config != NULL) {
        prefs = wv_send0(id, config, wv_sel("preferences"));
        if (prefs != NULL) {
            sel_dev = wv_sel("_setDeveloperExtrasEnabled:");
            if (wv_send1(BOOL, prefs, wv_sel("respondsToSelector:"), sel_dev)) {
                wv_send1(void, prefs, sel_dev, (BOOL)(enabled ? 1 : 0));
            }
        }
    }
    wv_pool_pop(pool);
    return 1;
}

int alya_webview_set_js(alya_webview_t *w, int enabled) {
    id pool;
    id config;
    id prefs;
    if (w == NULL || w->view == NULL) {
        return 0;
    }
    pool = wv_pool_push();
    config = wv_send0(id, w->view, wv_sel("configuration"));
    if (config != NULL) {
        prefs = wv_send0(id, config, wv_sel("preferences"));
        if (prefs != NULL) {
            SEL sel_js = wv_sel("setJavaScriptEnabled:");
            if (wv_send1(BOOL, prefs, wv_sel("respondsToSelector:"), sel_js)) {
                wv_send1(void, prefs, sel_js, (BOOL)(enabled ? 1 : 0));
            }
        }
        // macOS 11.0+: defaultWebpagePreferences.allowsContentJavaScript
        SEL sel_wp = wv_sel("defaultWebpagePreferences");
        if (wv_send1(BOOL, config, wv_sel("respondsToSelector:"), sel_wp)) {
            id wp = wv_send0(id, config, sel_wp);
            if (wp != NULL) {
                SEL sel_acjs = wv_sel("setAllowsContentJavaScript:");
                if (wv_send1(BOOL, wp, wv_sel("respondsToSelector:"), sel_acjs)) {
                    wv_send1(void, wp, sel_acjs, (BOOL)(enabled ? 1 : 0));
                }
            }
        }
    }
    wv_pool_pop(pool);
    return 1;
}

int alya_webview_set_user_agent(alya_webview_t *w, const char *ua) {
    id pool;
    if (w == NULL || w->view == NULL) {
        return 0;
    }
    if (ua == NULL) {
        ua = "";
    }
    pool = wv_pool_push();
    wv_send1(void, w->view, wv_sel("setCustomUserAgent:"), wv_nsstr(ua));
    wv_pool_pop(pool);
    return 1;
}

/* Synthetic input. Mouse and keyboard events are posted to our own
 * application queue (targeted at the embedded page, no system-wide
 * side effects, no Accessibility permission needed). The wheel uses a
 * CGEvent (global, applies to the focused view). Coordinates are
 * window client pixels, origin top-left. */

// CoreGraphics C API (typed prototypes, no ObjC involved).
typedef void *CGEventRef;
typedef void *CGEventSourceRef;
extern CGEventRef CGEventCreateScrollWheelEvent(CGEventSourceRef src,
                                                int unit,
                                                unsigned int wheels, int w1,
                                                ...);
extern void CGEventPost(int tap, CGEventRef ev);
extern void CFRelease(void *ref);

static double wv_uptime(void) {
    id pi = wv_send0(id, (id)objc_getClass("NSProcessInfo"),
                     wv_sel("processInfo"));
    if (pi == NULL) {
        return 0.0;
    }
    return wv_msg_send_double(pi, wv_sel("systemUptime"));
}

static long wv_window_number(alya_webview_t *w) {
    if (w == NULL || w->win == NULL) {
        return 0;
    }
    return wv_send0(long, w->win, wv_sel("windowNumber"));
}

static int wv_post_mouse(alya_webview_t *w, unsigned long type, int x,
                         int y, long clicks) {
    id pool;
    id app;
    id ev;
    NSPoint loc;
    if (w == NULL || w->win == NULL) {
        return 0;
    }
    loc.x = (double)x;
    loc.y = (double)(w->height - y); // base coords: origin bottom-left
    pool = wv_pool_push();
    app = wv_send0(id, (id)objc_getClass("NSApplication"),
                   wv_sel("sharedApplication"));
    ev = wv_send9(id, (id)objc_getClass("NSEvent"),
                  wv_sel("mouseEventWithType:location:modifierFlags:timestamp:windowNumber:context:eventNumber:clickCount:pressure:"),
                  type, loc, (unsigned long)0, wv_uptime(),
                  wv_window_number(w), NULL, (long)0, clicks, (double)1.0);
    if (ev != NULL) {
        wv_send2(void, app, wv_sel("postEvent:atStart:"), ev, (BOOL)0);
    }
    wv_pool_pop(pool);
    return ev != NULL ? 1 : 0;
}

int alya_webview_mouse_move(alya_webview_t *w, int x, int y) {
    return wv_post_mouse(w, 5, x, y, 0);
}

static int wv_mouse_btn(alya_webview_t *w, int button, int down) {
    unsigned long t = 0;
    if (w == NULL) {
        return 0;
    }
    if (button == 0) {
        t = down ? 1 : 2;
    } else if (button == 1) {
        t = down ? 3 : 4;
    } else if (button == 2) {
        t = down ? 25 : 26;
    } else {
        return 0;
    }
    return wv_post_mouse(w, t, 0, 0, down ? 1 : 0);
}

int alya_webview_mouse_down(alya_webview_t *w, int button) {
    return wv_mouse_btn(w, button, 1);
}

int alya_webview_mouse_up(alya_webview_t *w, int button) {
    return wv_mouse_btn(w, button, 0);
}

int alya_webview_mouse_click(alya_webview_t *w, int button) {
    int d;
    int u;
    if (w == NULL || button < 0 || button > 2) {
        return 0;
    }
    d = wv_mouse_btn(w, button, 1);
    u = wv_mouse_btn(w, button, 0);
    return (d && u) ? 1 : 0;
}

int alya_webview_mouse_wheel(alya_webview_t *w, int dx, int dy) {
    CGEventRef ev;
    if (w == NULL) {
        return 0;
    }
    if (dx == 0 && dy == 0) {
        return 0;
    }
    ev = CGEventCreateScrollWheelEvent(NULL, 0, 2, dy, dx);
    if (ev == NULL) {
        return 0;
    }
    CGEventPost(0, ev);
    CFRelease(ev);
    return 1;
}

static int wv_post_key(alya_webview_t *w, int code, const char *chars,
                       int down) {
    id pool;
    id app;
    id ev;
    NSPoint loc;
    if (w == NULL || w->win == NULL) {
        return 0;
    }
    if (chars == NULL) {
        chars = "";
    }
    loc.x = 0;
    loc.y = 0;
    pool = wv_pool_push();
    app = wv_send0(id, (id)objc_getClass("NSApplication"),
                   wv_sel("sharedApplication"));
    ev = wv_send10(id, (id)objc_getClass("NSEvent"),
                   wv_sel("keyEventWithType:location:modifierFlags:timestamp:windowNumber:context:characters:charactersIgnoringModifiers:isARepeat:keyCode:"),
                   (unsigned long)(down ? 10 : 11), loc,
                   (unsigned long)0, wv_uptime(), wv_window_number(w),
                   NULL, wv_nsstr(chars), wv_nsstr(chars), (BOOL)0,
                   (unsigned short)(code & 0xFFFF));
    if (ev != NULL) {
        wv_send2(void, app, wv_sel("postEvent:atStart:"), ev, (BOOL)0);
    }
    wv_pool_pop(pool);
    return ev != NULL ? 1 : 0;
}

int alya_webview_key_down(alya_webview_t *w, int code) {
    if (code <= 0) {
        return 0;
    }
    return wv_post_key(w, code, "", 1);
}

int alya_webview_key_up(alya_webview_t *w, int code) {
    if (code <= 0) {
        return 0;
    }
    return wv_post_key(w, code, "", 0);
}

int alya_webview_key_tap(alya_webview_t *w, int code) {
    int d;
    int u;
    if (w == NULL || code <= 0) {
        return 0;
    }
    d = wv_post_key(w, code, "", 1);
    u = wv_post_key(w, code, "", 0);
    return (d && u) ? 1 : 0;
}

int alya_webview_key_text(alya_webview_t *w, const char *text) {
    int ok = 0;
    const unsigned char *p;
    char one[5];
    if (w == NULL || text == NULL || text[0] == '\0') {
        return 0;
    }
    // One key event per UTF-8 code point.
    p = (const unsigned char *)text;
    while (*p != '\0') {
        int n = 1;
        if ((*p & 0x80) == 0) {
            n = 1;
        } else if ((*p & 0xE0) == 0xC0) {
            n = 2;
        } else if ((*p & 0xF0) == 0xE0) {
            n = 3;
        } else if ((*p & 0xF8) == 0xF0) {
            n = 4;
        }
        if (n > 4) {
            n = 1;
        }
        memcpy(one, p, (size_t)n);
        one[n] = '\0';
        if (wv_post_key(w, 0, one, 1)) {
            ok = 1;
        }
        wv_post_key(w, 0, one, 0);
        p += n;
    }
    return ok;
}

int alya_webview_key_code(const char *name) {
    static const struct {
        const char *name;
        int code;
    } map[] = {{"Enter", 0x24},     {"Escape", 0x35},
               {"Tab", 0x30},       {"Backspace", 0x33},
               {"Delete", 0x75},    {"Left", 0x7B},
               {"Up", 0x7E},        {"Right", 0x7C},
               {"Down", 0x7D},      {"Home", 0x73},
               {"End", 0x77},       {"PageUp", 0x74},
               {"PageDown", 0x79},  {NULL, -1}};
    int i;
    if (name == NULL) {
        return -1;
    }
    for (i = 0; map[i].name != NULL; i++) {
        if (strcmp(name, map[i].name) == 0) {
            return map[i].code;
        }
    }
    return -1;
}

int alya_webview_set_zoom(alya_webview_t *w, double factor) {
    id pool;
    NSPoint at;
    if (w == NULL || w->view == NULL) {
        return 0;
    }
#if defined(__x86_64__)
    {
        double real_factor;
        __asm__("movsd %%xmm1, %0" : "=x"(real_factor));
        factor = real_factor;
    }
#endif
    if (factor < 0.25 || factor > 5.0) {
        return 0;
    }
    at.x = 0;
    at.y = 0;
    pool = wv_pool_push();
    wv_send2(void, w->view, wv_sel("setMagnification:centeredAtPoint:"),
             factor, at);
    wv_pool_pop(pool);
    return 1;
}

double alya_webview_get_zoom(alya_webview_t *w) {
    id pool;
    double z = 0.0;
    if (w == NULL || w->view == NULL) {
        return 0.0;
    }
    pool = wv_pool_push();
    z = wv_msg_send_double(w->view, wv_sel("magnification"));
    wv_pool_pop(pool);
    return z;
}

/* Content-policy user scripts (document start, all frames). */
static const char *wv_script_shortcuts =
    "(function(){document.addEventListener('keydown',function(e){"
    "var k=e.key||'';"
    "if((e.ctrlKey&&(k==='p'||k==='P'))||k==='PrintScreen'||k==='F12'||"
    "((e.ctrlKey||e.metaKey)&&e.shiftKey&&(k==='I'||k==='J'||k==='C'||k==='i'||k==='j'||k==='c'))||"
    "((e.ctrlKey||e.metaKey)&&(k==='u'||k==='U'))){e.preventDefault();e.stopPropagation();}"
    "},true);})();";
static const char *wv_script_nomenu =
    "(function(){document.addEventListener('contextmenu',function(e){"
    "e.preventDefault();e.stopPropagation();"
    "},true);})();";

static void wv_refresh_scripts(alya_webview_t *w) {
    id pool;
    id config;
    id ucc;
    if (w == NULL || w->view == NULL) {
        return;
    }
    pool = wv_pool_push();
    config = wv_send0(id, w->view, wv_sel("configuration"));
    ucc = wv_send0(id, config, wv_sel("userContentController"));
    wv_send0(void, ucc, wv_sel("removeAllUserScripts"));
    if (!w->allow_menu) {
        id s = wv_send0(id, (id)objc_getClass("WKUserScript"),
                        wv_sel("alloc"));
        s = wv_send3(id, s,
                     wv_sel("initWithSource:injectionTime:forMainFrameOnly:"),
                     wv_nsstr(wv_script_nomenu), (long)0, (BOOL)0);
        wv_send1(void, ucc, wv_sel("addUserScript:"), s);
        wv_send0(void, s, wv_sel("release"));
    }
    if (w->block_keys) {
        id s = wv_send0(id, (id)objc_getClass("WKUserScript"),
                        wv_sel("alloc"));
        s = wv_send3(id, s,
                     wv_sel("initWithSource:injectionTime:forMainFrameOnly:"),
                     wv_nsstr(wv_script_shortcuts), (long)0, (BOOL)0);
        wv_send1(void, ucc, wv_sel("addUserScript:"), s);
        wv_send0(void, s, wv_sel("release"));
    }
    wv_pool_pop(pool);
}

int alya_webview_set_background(alya_webview_t *w, int r, int g, int b,
                                int a) {
    // WKWebView honors page-background drawing through the
    // long-standing drawsBackground key: transparent pages (wallpapers)
    // set it to NO, opaque pages leave it on. The exact fill color
    // comes from page CSS, so components are intentionally unused.
    id pool;
    id no;
    (void)r;
    (void)g;
    (void)b;
    if (w == NULL || w->view == NULL) {
        return 0;
    }
    pool = wv_pool_push();
    no = wv_send1(id, (id)objc_getClass("NSNumber"),
                  wv_sel("numberWithBool:"), (BOOL)(a >= 128 ? 1 : 0));
    SEL sel_db = wv_sel("setDrawsBackground:");
    if (wv_send1(BOOL, w->view, wv_sel("respondsToSelector:"), sel_db)) {
        wv_send1(void, w->view, sel_db, (BOOL)(a >= 128 ? 1 : 0));
    } else {
        SEL sel_kvc = wv_sel("setValue:forKey:");
        if (wv_send1(BOOL, w->view, wv_sel("respondsToSelector:"), sel_kvc)) {
            wv_send2(void, w->view, sel_kvc, no, wv_nsstr("drawsBackground"));
        }
    }
    wv_pool_pop(pool);
    return 1;
}

int alya_webview_set_context_menu(alya_webview_t *w, int enabled) {
    if (w == NULL || w->view == NULL) {
        return 0;
    }
    w->allow_menu = enabled ? 1 : 0;
    wv_refresh_scripts(w);
    return 1;
}

int alya_webview_set_shortcut_block(alya_webview_t *w, int enabled) {
    if (w == NULL || w->view == NULL) {
        return 0;
    }
    w->block_keys = enabled ? 1 : 0;
    wv_refresh_scripts(w);
    return 1;
}

int alya_webview_set_images(alya_webview_t *w, int enabled) {
    // No per-page image toggle in WKPreferences; report unsupported.
    (void)w;
    (void)enabled;
    return 0;
}

int alya_webview_set_webgl(alya_webview_t *w, int enabled) {
    (void)w;
    (void)enabled;
    return 0;
}

int alya_webview_set_charset(alya_webview_t *w, const char *cs) {
    (void)w;
    (void)cs;
    return 0;
}

int alya_webview_set_borderless(alya_webview_t *w, int enabled) {
    id pool;
    if (w == NULL || w->win == NULL) {
        return 0;
    }
    pool = wv_pool_push();
    if (enabled) {
        wv_send1(void, w->win, wv_sel("setStyleMask:"), (unsigned long)0);
    } else {
        wv_send1(void, w->win, wv_sel("setStyleMask:"), w->orig_mask);
    }
    wv_pool_pop(pool);
    return 1;
}

int alya_webview_set_topmost(alya_webview_t *w, int enabled) {
    id pool;
    if (w == NULL || w->win == NULL) {
        return 0;
    }
    pool = wv_pool_push();
    // NSFloatingWindowLevel = 3, NSNormalWindowLevel = 0.
    wv_send1(void, w->win, wv_sel("setLevel:"), (long)(enabled ? 3 : 0));
    wv_pool_pop(pool);
    return 1;
}

int alya_webview_set_opacity(alya_webview_t *w, double alpha) {
    id pool;
    if (w == NULL || w->win == NULL) {
        return 0;
    }
#if defined(__x86_64__)
    {
        double real_alpha;
        __asm__("movsd %%xmm1, %0" : "=x"(real_alpha));
        alpha = real_alpha;
    }
#endif
    if (alpha < 0.0 || alpha > 1.0) {
        return 0;
    }
    pool = wv_pool_push();
    wv_send1(void, w->win, wv_sel("setAlphaValue:"), alpha);
    wv_send1(void, w->win, wv_sel("setOpaque:"), (BOOL)(alpha >= 1.0 ? 1 : 0));
    wv_pool_pop(pool);
    return 1;
}

int alya_webview_set_click_through(alya_webview_t *w, int enabled) {
    id pool;
    if (w == NULL || w->win == NULL) {
        return 0;
    }
    pool = wv_pool_push();
    wv_send1(void, w->win, wv_sel("setIgnoresMouseEvents:"),
             (BOOL)(enabled ? 1 : 0));
    wv_pool_pop(pool);
    return 1;
}

int alya_webview_set_fullscreen(alya_webview_t *w, int enabled) {
    id pool;
    if (w == NULL || w->win == NULL) {
        return 0;
    }
    if ((enabled ? 1 : 0) == w->is_fullscreen) {
        return 1;
    }
    pool = wv_pool_push();
    wv_send1(void, w->win, wv_sel("toggleFullScreen:"), NULL);
    wv_pool_pop(pool);
    w->is_fullscreen = enabled ? 1 : 0;
    return 1;
}

int alya_webview_focus(alya_webview_t *w) {
    id pool;
    if (w == NULL || w->win == NULL) {
        return 0;
    }
    pool = wv_pool_push();
    wv_send1(void, w->win, wv_sel("makeKeyAndOrderFront:"), NULL);
    if (w->view != NULL) {
        wv_send1(void, w->win, wv_sel("makeFirstResponder:"), w->view);
    }
    wv_pool_pop(pool);
    return 1;
}

int alya_webview_minimize(alya_webview_t *w) {
    id pool;
    if (w == NULL || w->win == NULL) {
        return 0;
    }
    pool = wv_pool_push();
    wv_send1(void, w->win, wv_sel("miniaturize:"), NULL);
    wv_pool_pop(pool);
    return 1;
}

int alya_webview_restore(alya_webview_t *w) {
    id pool;
    if (w == NULL || w->win == NULL) {
        return 0;
    }
    pool = wv_pool_push();
    wv_send1(void, w->win, wv_sel("deminiaturize:"), NULL);
    wv_pool_pop(pool);
    return 1;
}

static double wv_screen_h(void) {
    id screen = wv_send0(id, (id)objc_getClass("NSScreen"),
                         wv_sel("mainScreen"));
    NSRect fr;
    if (screen == NULL) {
        return 800.0;
    }
#if defined(__x86_64__)
    objc_msgSend_stret(&fr, screen, wv_sel("frame"));
#elif defined(__aarch64__) || defined(__arm64__)
    fr = ((NSRect (*)(id, SEL))objc_msgSend)(screen, wv_sel("frame"));
#else
    fr.h = 800.0;
#endif
    return fr.h;
}

int alya_webview_set_position(alya_webview_t *w, int x, int y) {
    id pool;
    NSPoint origin;
    if (w == NULL || w->win == NULL) {
        return 0;
    }
    // Screen coords origin bottom-left: flip the client y.
    origin.x = (double)x;
    origin.y = wv_screen_h() - (double)y - (double)w->height;
    pool = wv_pool_push();
    wv_send1(void, w->win, wv_sel("setFrameOrigin:"), origin);
    wv_pool_pop(pool);
    return 1;
}

int alya_webview_stop(alya_webview_t *w) {
    id pool;
    if (w == NULL || w->view == NULL) {
        return 0;
    }
    pool = wv_pool_push();
    wv_send0(void, w->view, wv_sel("stopLoading"));
    wv_pool_pop(pool);
    return 1;
}

int alya_webview_reload_bypass(alya_webview_t *w) {
    id pool;
    if (w == NULL || w->view == NULL) {
        return 0;
    }
    pool = wv_pool_push();
    wv_send0(void, w->view, wv_sel("reloadFromOrigin"));
    wv_pool_pop(pool);
    return 1;
}

int alya_webview_poll(alya_webview_t *w) {
    int kind;
    id pool;
    id app;
    id distant;
    id mode;
    id ev;
    if (w == NULL) {
        return ALYA_WEBVIEW_EVENT_NONE;
    }
    pool = wv_pool_push();
    app = wv_send0(id, (id)objc_getClass("NSApplication"),
                   wv_sel("sharedApplication"));
    distant = wv_send0(id, (id)objc_getClass("NSDate"),
                       wv_sel("distantPast"));
    mode = wv_nsstr("kCFRunLoopDefaultMode");
    for (;;) {
        ev = wv_send4(id, app,
                      wv_sel("nextEventMatchingMask:untilDate:inMode:dequeue:"),
                      (unsigned long long)0xFFFFFFFFFFFFFFFFULL, distant,
                      mode, (long long)1);
        if (ev == NULL) {
            break;
        }
        wv_send1(void, app, wv_sel("sendEvent:"), ev);
    }
    wv_pool_pop(pool);
    if (w->head == w->tail) {
        return ALYA_WEBVIEW_EVENT_NONE;
    }
    kind = w->queue[w->head].kind;
    w->width = w->queue[w->head].w;
    w->height = w->queue[w->head].h;
    wv_copy(w->url, sizeof(w->url), w->queue[w->head].url);
    if (kind == ALYA_WEBVIEW_EVENT_MESSAGE) {
        wv_copy(w->message, sizeof(w->message), w->queue[w->head].text);
    } else if (kind == ALYA_WEBVIEW_EVENT_TITLE) {
        wv_copy(w->title, sizeof(w->title), w->queue[w->head].text);
    }
    w->head = (w->head + 1) % WV_MAX_EVENTS;
    return kind;
}

int alya_webview_event_width(alya_webview_t *w) {
    if (w == NULL) {
        return 0;
    }
    return w->width;
}

int alya_webview_event_height(alya_webview_t *w) {
    if (w == NULL) {
        return 0;
    }
    return w->height;
}

const char *alya_webview_event_url(alya_webview_t *w) {
    if (w == NULL) {
        return "";
    }
    return w->url;
}

const char *alya_webview_event_title(alya_webview_t *w) {
    if (w == NULL) {
        return "";
    }
    return w->title;
}

const char *alya_webview_event_text(alya_webview_t *w) {
    if (w == NULL) {
        return "";
    }
    return w->message;
}

const char *alya_webview_eval_result(alya_webview_t *w) {
    if (w == NULL) {
        return "";
    }
    return w->eval_result;
}
