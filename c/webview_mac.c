// macOS backend for the Alya webview package: WKWebView.
//
// Pure C over the Objective-C runtime C API (same technique as the gui
// package's cocoa_window.c): no ObjC syntax, so this file needs no special
// compiler mode and links only against system frameworks (Cocoa + WebKit).
// The JS completion handler is a malloc'd stack-block literal; it frees
// itself when it fires (or on timeout bookkeeping in eval_state).
//
// Threading: all WebKit work happens on the calling (main) thread while
// poll() spins the runloop non-blocking. No worker threads are created.

#include "webview.h"

#include <stdlib.h>
#include <string.h>

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
extern void *_NSConcreteStackBlock;

typedef struct NSRect {
    double x;
    double y;
    double w;
    double h;
} NSRect;

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
    return objc_msgSend((id)objc_getClass("NSString"),
                        wv_sel("stringWithUTF8String:"), s);
}

static const char *wv_cstr(id nsstr) {
    if (nsstr == NULL) {
        return "";
    }
    return (const char *)objc_msgSend(nsstr, wv_sel("UTF8String"));
}

static void wv_push(alya_webview_t *w, int kind) {
    int next;
    if (w == NULL) {
        return;
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
    url = objc_msgSend(view, wv_sel("URL"));
    if (url == NULL) {
        return;
    }
    abs = objc_msgSend(url, wv_sel("absoluteString"));
    wv_copy(w->url, sizeof(w->url), wv_cstr(abs));
}

static void wv_sync_title(alya_webview_t *w) {
    id t;
    if (w == NULL || w->view == NULL) {
        return;
    }
    t = objc_msgSend(w->view, wv_sel("title"));
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
    wv_push(w, ALYA_WEBVIEW_EVENT_NAV_DONE);
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
    body = objc_msgSend(message, wv_sel("body"));
    if (body == NULL) {
        return;
    }
    desc = objc_msgSend(body, wv_sel("description"));
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

/* --- JS completion block (malloc'd, frees itself when it fires) --- */

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
} wv_block_t;

static wv_blk_desc_t wv_blk_desc = {0, sizeof(wv_block_t)};

static void wv_js_invoke(void *blk, id result, id error) {
    wv_block_t *b = (wv_block_t *)blk;
    alya_webview_t *w = b->w;
    if (w != NULL) {
        if (error == NULL && result != NULL) {
            id desc = objc_msgSend(result, wv_sel("description"));
            wv_copy(w->eval_result, sizeof(w->eval_result),
                    wv_cstr(desc));
            w->eval_state = ALYA_WEBVIEW_EVAL_READY;
        } else {
            w->eval_result[0] = '\0';
            w->eval_state = ALYA_WEBVIEW_EVAL_ERROR;
        }
        w->eval_pending = 0;
    }
    free(b);
}

static int wv_fire_js(alya_webview_t *w, const char *js) {
    wv_block_t *b;
    id code;
    if (w == NULL || w->view == NULL) {
        return 0;
    }
    if (js == NULL || js[0] == '\0') {
        return 0;
    }
    b = (wv_block_t *)malloc(sizeof(*b));
    if (b == NULL) {
        return 0;
    }
    b->isa = _NSConcreteStackBlock;
    b->flags = 0;
    b->reserved = 0;
    b->invoke = wv_js_invoke;
    b->desc = &wv_blk_desc;
    b->w = w;
    code = wv_nsstr(js);
    w->eval_state = ALYA_WEBVIEW_EVAL_PENDING;
    w->eval_result[0] = '\0';
    w->eval_pending = 1;
    objc_msgSend(w->view, wv_sel("evaluateJavaScript:completionHandler:"),
                 code, (id)b);
    return 1;
}

/* --- autorelease pool helpers (no ObjC syntax available) --- */

static id wv_pool_push(void) {
    id pool = objc_msgSend((id)objc_getClass("NSAutoreleasePool"),
                           wv_sel("alloc"));
    return objc_msgSend(pool, wv_sel("init"));
}

static void wv_pool_pop(id pool) {
    if (pool != NULL) {
        objc_msgSend(pool, wv_sel("drain"));
    }
}

/* ---------------- public contract ---------------- */

int alya_webview_backend_id(void) {
    return 2;
}

const char *alya_webview_backend_name(void) {
    return "macos";
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
    ws = objc_msgSend((id)objc_getClass("NSWorkspace"),
                      wv_sel("sharedWorkspace"));
    if (ws != NULL) {
        nsurl = objc_msgSend((id)objc_getClass("NSURL"),
                             wv_sel("URLWithString:"), wv_nsstr(url));
        if (nsurl != NULL) {
            ok = (long)objc_msgSend(ws, wv_sel("openURL:"), nsurl);
        }
    }
    wv_pool_pop(pool);
    return ok ? 1 : 0;
}

alya_webview_t *alya_webview_create(const char *title, int width,
                                    int height) {
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
    w->width = width;
    w->height = height;
    wv_copy(w->title, sizeof(w->title), title);

    pool = wv_pool_push();
    app = objc_msgSend((id)objc_getClass("NSApplication"),
                       wv_sel("sharedApplication"));
    if (app == NULL) {
        wv_pool_pop(pool);
        free(w);
        return NULL;
    }
    objc_msgSend(app, wv_sel("setActivationPolicy:"), (long)0);
    objc_msgSend(app, wv_sel("finishLaunching"));

    rect.x = 100;
    rect.y = 100;
    rect.w = (double)width;
    rect.h = (double)height;
    win = objc_msgSend((id)objc_getClass("NSWindow"), wv_sel("alloc"));
    win = objc_msgSend(win, wv_sel("initWithContentRect:styleMask:backing:defer:"),
                       rect, (unsigned long)15, (unsigned long)2, (BOOL)0);
    if (win == NULL) {
        wv_pool_pop(pool);
        free(w);
        return NULL;
    }
    objc_msgSend(win, wv_sel("setTitle:"), wv_nsstr(title));
    objc_msgSend(win, wv_sel("setReleasedWhenClosed:"), (BOOL)0);

    config = objc_msgSend((id)objc_getClass("WKWebViewConfiguration"),
                          wv_sel("alloc"));
    config = objc_msgSend(config, wv_sel("init"));
    ucc = objc_msgSend(config, wv_sel("userContentController"));

    del = objc_msgSend((id)wv_delegate_class(), wv_sel("alloc"));
    del = objc_msgSend(del, wv_sel("init"));
    iv = class_getInstanceVariable(wv_delegate_class(), "wvCtx");
    object_setIvar(del, iv, (id)(void *)w);

    objc_msgSend(ucc, wv_sel("addScriptMessageHandler:name:"), del,
                 wv_nsstr("alya"));

    content = objc_msgSend(win, wv_sel("contentView"));
    {
        // Start at the requested size (kept in sync on resize).
        NSRect fr;
        fr.x = 0;
        fr.y = 0;
        fr.w = (double)width;
        fr.h = (double)height;
        view = objc_msgSend((id)objc_getClass("WKWebView"), wv_sel("alloc"));
        view = objc_msgSend(view, wv_sel("initWithFrame:configuration:"),
                            fr, config);
    }
    if (view == NULL) {
        objc_msgSend(del, wv_sel("release"));
        objc_msgSend(config, wv_sel("release"));
        objc_msgSend(win, wv_sel("release"));
        wv_pool_pop(pool);
        free(w);
        return NULL;
    }
    objc_msgSend(view, wv_sel("setNavigationDelegate:"), del);
    objc_msgSend(view, wv_sel("setAutoresizingMask:"),
                 (unsigned long)(2 | 16));
    objc_msgSend(content, wv_sel("addSubview:"), view);
    objc_msgSend(view, wv_sel("addObserver:forKeyPath:options:context:"),
                 del, wv_nsstr("title"), (unsigned long)0, NULL);
    objc_msgSend(win, wv_sel("setDelegate:"), del);

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
        objc_msgSend(w->view,
                     wv_sel("removeObserver:forKeyPath:"), w->delegate,
                     wv_nsstr("title"));
        objc_msgSend(w->view, wv_sel("setNavigationDelegate:"), NULL);
        objc_msgSend(w->view, wv_sel("removeFromSuperview"));
        objc_msgSend(w->view, wv_sel("release"));
        w->view = NULL;
    }
    if (w->win != NULL) {
        objc_msgSend(w->win, wv_sel("setDelegate:"), NULL);
        objc_msgSend(w->win, wv_sel("close"));
        objc_msgSend(w->win, wv_sel("release"));
        w->win = NULL;
    }
    if (w->delegate != NULL) {
        objc_msgSend(w->delegate, wv_sel("release"));
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
    objc_msgSend(w->win, wv_sel("makeKeyAndOrderFront:"), NULL);
    {
        id app = objc_msgSend((id)objc_getClass("NSApplication"),
                              wv_sel("sharedApplication"));
        objc_msgSend(app, wv_sel("activateIgnoringOtherApps:"), (BOOL)1);
    }
    wv_pool_pop(pool);
}

void alya_webview_hide(alya_webview_t *w) {
    id pool;
    if (w == NULL || w->win == NULL) {
        return;
    }
    pool = wv_pool_push();
    objc_msgSend(w->win, wv_sel("orderOut:"), NULL);
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
        objc_msgSend(w->win, wv_sel("close"));
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
    objc_msgSend(w->win, wv_sel("setTitle:"), wv_nsstr(title));
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
    objc_msgSend(w->win, wv_sel("setContentSize:"), size);
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
    nsurl = objc_msgSend((id)objc_getClass("NSURL"),
                         wv_sel("URLWithString:"), wv_nsstr(url));
    if (nsurl == NULL) {
        wv_pool_pop(pool);
        return 0;
    }
    req = objc_msgSend((id)objc_getClass("NSURLRequest"),
                       wv_sel("requestWithURL:"), nsurl);
    objc_msgSend(w->view, wv_sel("loadRequest:"), req);
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
    objc_msgSend(w->view, wv_sel("loadHTMLString:baseURL:"), wv_nsstr(html),
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
    objc_msgSend(w->view, wv_sel("reload"));
    wv_pool_pop(pool);
    return 1;
}

int alya_webview_go_back(alya_webview_t *w) {
    id pool;
    if (w == NULL || w->view == NULL) {
        return 0;
    }
    pool = wv_pool_push();
    objc_msgSend(w->view, wv_sel("goBack"));
    wv_pool_pop(pool);
    return 1;
}

int alya_webview_go_forward(alya_webview_t *w) {
    id pool;
    if (w == NULL || w->view == NULL) {
        return 0;
    }
    pool = wv_pool_push();
    objc_msgSend(w->view, wv_sel("goForward"));
    wv_pool_pop(pool);
    return 1;
}

int alya_webview_can_back(alya_webview_t *w) {
    id pool;
    long v = 0;
    if (w == NULL || w->view == NULL) {
        return 0;
    }
    pool = wv_pool_push();
    v = (long)objc_msgSend(w->view, wv_sel("canGoBack"));
    wv_pool_pop(pool);
    return v ? 1 : 0;
}

int alya_webview_can_forward(alya_webview_t *w) {
    id pool;
    long v = 0;
    if (w == NULL || w->view == NULL) {
        return 0;
    }
    pool = wv_pool_push();
    v = (long)objc_msgSend(w->view, wv_sel("canGoForward"));
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
        objc_msgSend(w->view,
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
    if (w == NULL || w->view == NULL) {
        return 0;
    }
    pool = wv_pool_push();
    config = objc_msgSend(w->view, wv_sel("configuration"));
    prefs = objc_msgSend(config, wv_sel("preferences"));
    objc_msgSend(prefs, wv_sel("setDeveloperExtrasEnabled:"),
                 (BOOL)(enabled ? 1 : 0));
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
    config = objc_msgSend(w->view, wv_sel("configuration"));
    prefs = objc_msgSend(config, wv_sel("preferences"));
    // javaScriptEnabled is get-only on modern WebKit; the set may no-op
    // on newer systems but is harmless to attempt.
    objc_msgSend(prefs, wv_sel("setJavaScriptEnabled:"),
                 (BOOL)(enabled ? 1 : 0));
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
    objc_msgSend(w->view, wv_sel("setCustomUserAgent:"), wv_nsstr(ua));
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
    app = objc_msgSend((id)objc_getClass("NSApplication"),
                       wv_sel("sharedApplication"));
    distant = objc_msgSend((id)objc_getClass("NSDate"),
                           wv_sel("distantPast"));
    mode = wv_nsstr("kCFRunLoopDefaultMode");
    for (;;) {
        ev = objc_msgSend(app,
                          wv_sel("nextEventMatchingMask:untilDate:inMode:dequeue:"),
                          (unsigned long long)0xFFFFFFFFFFFFFFFFULL, distant,
                          mode, (long long)1);
        if (ev == NULL) {
            break;
        }
        objc_msgSend(app, wv_sel("sendEvent:"), ev);
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
