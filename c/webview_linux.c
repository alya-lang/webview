// Linux backend for the Alya webview package: WebKitGTK.
//
// No compile-time dependency on GTK/WebKit headers: every symbol is
// resolved at runtime with dlopen()/dlsym() (only -ldl is linked). When
// the libraries, a display, or initialization are missing, create()
// returns NULL and every other entry point degrades to a null-safe
// no-op, which keeps headless CI green without extra system packages.
//
// Supported engines, probed in order:
//   - libwebkit2gtk-4.1 + libjavascriptcoregtk-4.1 (Ubuntu 22.10+)
//   - libwebkit2gtk-4.0 + libjavascriptcoregtk-4.0 (older LTS)
// The 4.1 async eval API is preferred; 4.0 run_javascript is the fallback.
//
// Threading: GTK is driven from the calling thread; poll() iterates
// pending events without blocking. No worker threads are created.

#include "webview.h"

#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

#define WV_MAX_EVENTS 64

typedef struct {
    int kind;
    int w;
    int h;
    char url[ALYA_WEBVIEW_URL_CAP];
    char text[ALYA_WEBVIEW_TEXT_CAP];
} wv_event_t;

/* Opaque handle types from the dlopen'd libraries. */
typedef struct _GtkWidget GtkWidget;
typedef struct _GObject GObject;
typedef struct _GParamSpec GParamSpec;
typedef struct _GAsyncResult GAsyncResult;
typedef struct _GError {
    unsigned int domain;
    int code;
    char *message;
} GError;
typedef void (*GAsyncReadyCallback)(GObject *source, GAsyncResult *res,
                                   void *user_data);
typedef void (*GClosureNotify)(void *data, void *closure);
typedef unsigned long gulong;
typedef int gboolean;
typedef void *gpointer;

/* JS eval states muddy the two API generations behind one flag. */
#define WV_API_NONE 0
#define WV_API_41 1
#define WV_API_40 2

struct alya_webview {
    GtkWidget *win;
    GtkWidget *view;
    int open;
    int ready;
    int api;
    int width;
    int height;
    int eval_state;
    char url[ALYA_WEBVIEW_URL_CAP];
    char title[ALYA_WEBVIEW_TEXT_CAP];
    char message[ALYA_WEBVIEW_TEXT_CAP];
    char eval_result[ALYA_WEBVIEW_TEXT_CAP];
    int head;
    int tail;
    wv_event_t queue[WV_MAX_EVENTS];
};

/* --- dynamically resolved symbols --- */

static void *wv_h_gtk;
static void *wv_h_webkit;
static void *wv_h_gobject;
static void *wv_h_glib;
static void *wv_h_jsc;

#define WV_DECL(ret, name, ...) static ret (*p_##name)(__VA_ARGS__)

WV_DECL(int, gtk_init_check, int *argc, char ***argv);
WV_DECL(GtkWidget *, gtk_window_new, int type);
WV_DECL(void, gtk_window_set_title, GtkWidget *w, const char *t);
WV_DECL(void, gtk_window_set_default_size, GtkWidget *w, int x, int y);
WV_DECL(void, gtk_window_resize, GtkWidget *w, int x, int y);
WV_DECL(void, gtk_widget_show_all, GtkWidget *w);
WV_DECL(void, gtk_widget_hide, GtkWidget *w);
WV_DECL(void, gtk_widget_destroy, GtkWidget *w);
WV_DECL(void, gtk_container_add, GtkWidget *c, GtkWidget *w);
WV_DECL(int, gtk_events_pending, void);
WV_DECL(int, gtk_main_iteration_do, int blocking);
WV_DECL(gulong, g_signal_connect_data, void *inst, const char *sig,
        void *handler, void *data, void *closure, int flags);
WV_DECL(void *, webkit_web_view_new, void);
WV_DECL(void, webkit_web_view_load_uri, GtkWidget *v, const char *uri);
WV_DECL(void, webkit_web_view_load_html, GtkWidget *v, const char *html,
        const char *base_uri);
WV_DECL(void, webkit_web_view_reload, GtkWidget *v);
WV_DECL(void, webkit_web_view_go_back, GtkWidget *v);
WV_DECL(void, webkit_web_view_go_forward, GtkWidget *v);
WV_DECL(int, webkit_web_view_can_go_back, GtkWidget *v);
WV_DECL(int, webkit_web_view_can_go_forward, GtkWidget *v);
WV_DECL(const char *, webkit_web_view_get_uri, GtkWidget *v);
WV_DECL(const char *, webkit_web_view_get_title, GtkWidget *v);
WV_DECL(void *, webkit_web_view_get_settings, GtkWidget *v);
WV_DECL(void *, webkit_web_view_get_user_content_manager, GtkWidget *v);
WV_DECL(void, webkit_settings_set_enable_javascript, void *s, int v);
WV_DECL(void, webkit_settings_set_enable_developer_extras, void *s, int v);
WV_DECL(void, webkit_settings_set_user_agent, void *s, const char *ua);
WV_DECL(int, webkit_user_content_manager_register_script_message_handler,
        void *m, const char *name);
WV_DECL(void, webkit_web_view_evaluate_javascript, GtkWidget *v,
        const char *script, int len, const char **exts, const char *world,
        const char *source_uri, void *cancellable, GAsyncReadyCallback cb,
        void *user_data);
WV_DECL(const char *, webkit_web_view_evaluate_javascript_finish,
        GtkWidget *v, GAsyncResult *res, void **error);
WV_DECL(void, webkit_web_view_run_javascript, GtkWidget *v,
        const char *script, void *cancellable, GAsyncReadyCallback cb,
        void *user_data);
WV_DECL(void *, webkit_web_view_run_javascript_finish, GtkWidget *v,
        GAsyncResult *res, void **error);
WV_DECL(void *, webkit_javascript_result_get_js_value, void *r);
WV_DECL(char *, jsc_value_to_string, void *v);
WV_DECL(void, g_free, void *p);
WV_DECL(void, g_error_free, void *e);

#define WV_LOAD(handle, name)                                             \
    do {                                                                  \
        p_##name = (void *)dlsym((handle), #name);                         \
        if (p_##name == NULL) {                                           \
            return 0;                                                     \
        }                                                                 \
    } while (0)

#define WV_LOAD_OPT(handle, name) \
    p_##name = (void *)dlsym((handle), #name)

static void wv_unload_all(void) {
    if (wv_h_jsc != NULL) {
        dlclose(wv_h_jsc);
        wv_h_jsc = NULL;
    }
    if (wv_h_webkit != NULL) {
        dlclose(wv_h_webkit);
        wv_h_webkit = NULL;
    }
    if (wv_h_gtk != NULL) {
        dlclose(wv_h_gtk);
        wv_h_gtk = NULL;
    }
    if (wv_h_gobject != NULL) {
        dlclose(wv_h_gobject);
        wv_h_gobject = NULL;
    }
    if (wv_h_glib != NULL) {
        dlclose(wv_h_glib);
        wv_h_glib = NULL;
    }
}

static int wv_load_all(void) {
    static int tried = 0;
    static int ok = 0;
    int i;
    static const char *gtk_names[] = {"libgtk-3.so.0", "libgtk-3.so",
                                      NULL};
    static const char *webkit_names[] = {"libwebkit2gtk-4.1.so.0",
                                         "libwebkit2gtk-4.0.so.37",
                                         "libwebkit2gtk-4.0.so", NULL};
    static const char *jsc_names[] = {
        "libjavascriptcoregtk-4.1.so.0", "libjavascriptcoregtk-4.0.so.18",
        "libjavascriptcoregtk-4.0.so", NULL};
    static const char *gobject_names[] = {"libgobject-2.0.so.0",
                                          "libgobject-2.0.so", NULL};
    static const char *glib_names[] = {"libglib-2.0.so.0", "libglib-2.0.so",
                                       NULL};
    if (tried) {
        return ok;
    }
    tried = 1;

    wv_h_gobject = NULL;
    for (i = 0; gobject_names[i] != NULL; i++) {
        wv_h_gobject = dlopen(gobject_names[i], RTLD_NOW | RTLD_GLOBAL);
        if (wv_h_gobject != NULL) {
            break;
        }
    }
    wv_h_glib = NULL;
    for (i = 0; glib_names[i] != NULL; i++) {
        wv_h_glib = dlopen(glib_names[i], RTLD_NOW | RTLD_GLOBAL);
        if (wv_h_glib != NULL) {
            break;
        }
    }
    wv_h_gtk = NULL;
    for (i = 0; gtk_names[i] != NULL; i++) {
        wv_h_gtk = dlopen(gtk_names[i], RTLD_NOW | RTLD_GLOBAL);
        if (wv_h_gtk != NULL) {
            break;
        }
    }
    wv_h_webkit = NULL;
    for (i = 0; webkit_names[i] != NULL; i++) {
        wv_h_webkit = dlopen(webkit_names[i], RTLD_NOW | RTLD_GLOBAL);
        if (wv_h_webkit != NULL) {
            break;
        }
    }
    wv_h_jsc = NULL;
    for (i = 0; jsc_names[i] != NULL; i++) {
        wv_h_jsc = dlopen(jsc_names[i], RTLD_NOW | RTLD_GLOBAL);
        if (wv_h_jsc != NULL) {
            break;
        }
    }
    if (wv_h_gtk == NULL || wv_h_webkit == NULL || wv_h_gobject == NULL ||
        wv_h_glib == NULL || wv_h_jsc == NULL) {
        wv_unload_all();
        ok = 0;
        return 0;
    }

    WV_LOAD(wv_h_gtk, gtk_init_check);
    WV_LOAD(wv_h_gtk, gtk_window_new);
    WV_LOAD(wv_h_gtk, gtk_window_set_title);
    WV_LOAD(wv_h_gtk, gtk_window_set_default_size);
    WV_LOAD(wv_h_gtk, gtk_window_resize);
    WV_LOAD(wv_h_gtk, gtk_widget_show_all);
    WV_LOAD(wv_h_gtk, gtk_widget_hide);
    WV_LOAD(wv_h_gtk, gtk_widget_destroy);
    WV_LOAD(wv_h_gtk, gtk_container_add);
    WV_LOAD(wv_h_gtk, gtk_events_pending);
    WV_LOAD(wv_h_gtk, gtk_main_iteration_do);
    WV_LOAD(wv_h_gobject, g_signal_connect_data);
    WV_LOAD(wv_h_webkit, webkit_web_view_new);
    WV_LOAD(wv_h_webkit, webkit_web_view_load_uri);
    WV_LOAD(wv_h_webkit, webkit_web_view_load_html);
    WV_LOAD(wv_h_webkit, webkit_web_view_reload);
    WV_LOAD(wv_h_webkit, webkit_web_view_go_back);
    WV_LOAD(wv_h_webkit, webkit_web_view_go_forward);
    WV_LOAD(wv_h_webkit, webkit_web_view_can_go_back);
    WV_LOAD(wv_h_webkit, webkit_web_view_can_go_forward);
    WV_LOAD(wv_h_webkit, webkit_web_view_get_uri);
    WV_LOAD(wv_h_webkit, webkit_web_view_get_title);
    WV_LOAD(wv_h_webkit, webkit_web_view_get_settings);
    WV_LOAD(wv_h_webkit, webkit_web_view_get_user_content_manager);
    WV_LOAD(wv_h_webkit, webkit_settings_set_enable_javascript);
    WV_LOAD(wv_h_webkit, webkit_settings_set_enable_developer_extras);
    WV_LOAD(wv_h_webkit, webkit_settings_set_user_agent);
    WV_LOAD(wv_h_webkit,
            webkit_user_content_manager_register_script_message_handler);
    WV_LOAD(wv_h_glib, g_free);
    WV_LOAD(wv_h_glib, g_error_free);
    WV_LOAD(wv_h_jsc, jsc_value_to_string);

    // Eval generations: 4.1 preferred, 4.0 fallback.
    WV_LOAD_OPT(wv_h_webkit, webkit_web_view_evaluate_javascript);
    WV_LOAD_OPT(wv_h_webkit, webkit_web_view_evaluate_javascript_finish);
    WV_LOAD_OPT(wv_h_webkit, webkit_web_view_run_javascript);
    WV_LOAD_OPT(wv_h_webkit, webkit_web_view_run_javascript_finish);
    WV_LOAD_OPT(wv_h_webkit, webkit_javascript_result_get_js_value);

    ok = 1;
    return 1;
}

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

static void wv_sync_url_title(alya_webview_t *w) {
    const char *u;
    const char *t;
    if (w == NULL || w->view == NULL) {
        return;
    }
    u = p_webkit_web_view_get_uri(w->view);
    if (u != NULL) {
        wv_copy(w->url, sizeof(w->url), u);
    }
    t = p_webkit_web_view_get_title(w->view);
    if (t != NULL) {
        wv_copy(w->title, sizeof(w->title), t);
    }
}

/* --- signal callbacks (C-to-C only, never into Alya) --- */

static void wv_on_load_changed(GtkWidget *view, int event, void *data) {
    alya_webview_t *w = (alya_webview_t *)data;
    if (w == NULL) {
        return;
    }
    if (event == 0) { // WEBKIT_LOAD_STARTED
        wv_sync_url_title(w);
        wv_push(w, ALYA_WEBVIEW_EVENT_NAV_START);
    } else if (event == 3) { // WEBKIT_LOAD_FINISHED
        wv_sync_url_title(w);
        wv_push(w, ALYA_WEBVIEW_EVENT_NAV_DONE);
    } else if (event == 4) { // WEBKIT_LOAD_FAILED
        wv_sync_url_title(w);
        wv_push(w, ALYA_WEBVIEW_EVENT_NAV_DONE);
    }
}

static void wv_on_title(GObject *obj, GParamSpec *pspec, void *data) {
    alya_webview_t *w = (alya_webview_t *)data;
    (void)obj;
    (void)pspec;
    if (w == NULL) {
        return;
    }
    wv_sync_url_title(w);
    wv_push(w, ALYA_WEBVIEW_EVENT_TITLE);
}

static void wv_on_script_message(void *manager, void *js_result,
                                 void *data) {
    alya_webview_t *w = (alya_webview_t *)data;
    void *jsv;
    char *s;
    (void)manager;
    if (w == NULL || js_result == NULL) {
        return;
    }
    if (p_webkit_javascript_result_get_js_value == NULL) {
        return;
    }
    jsv = p_webkit_javascript_result_get_js_value(js_result);
    if (jsv == NULL) {
        return;
    }
    s = p_jsc_value_to_string(jsv);
    if (s != NULL) {
        wv_copy(w->message, sizeof(w->message), s);
        p_g_free(s);
        wv_push(w, ALYA_WEBVIEW_EVENT_MESSAGE);
    }
}

static void wv_on_destroy(GtkWidget *win, void *data) {
    alya_webview_t *w = (alya_webview_t *)data;
    (void)win;
    if (w == NULL) {
        return;
    }
    w->open = 0;
    wv_push(w, ALYA_WEBVIEW_EVENT_CLOSE);
}

static void wv_on_eval_41(GObject *src, GAsyncResult *res, void *data) {
    alya_webview_t *w = (alya_webview_t *)data;
    void *err = NULL;
    const char *s;
    (void)src;
    if (w == NULL) {
        return;
    }
    if (p_webkit_web_view_evaluate_javascript_finish == NULL) {
        w->eval_result[0] = '\0';
        w->eval_state = ALYA_WEBVIEW_EVAL_ERROR;
        return;
    }
    s = p_webkit_web_view_evaluate_javascript_finish(w->view, res, &err);
    if (err != NULL) {
        p_g_error_free(err);
        w->eval_result[0] = '\0';
        w->eval_state = ALYA_WEBVIEW_EVAL_ERROR;
        return;
    }
    wv_copy(w->eval_result, sizeof(w->eval_result), s);
    if (s != NULL) {
        p_g_free((void *)s);
    }
    w->eval_state = ALYA_WEBVIEW_EVAL_READY;
}

static void wv_on_eval_40(GObject *src, GAsyncResult *res, void *data) {
    alya_webview_t *w = (alya_webview_t *)data;
    void *err = NULL;
    void *jsr;
    void *jsv;
    char *s;
    (void)src;
    if (w == NULL) {
        return;
    }
    if (p_webkit_web_view_run_javascript_finish == NULL ||
        p_webkit_javascript_result_get_js_value == NULL) {
        w->eval_result[0] = '\0';
        w->eval_state = ALYA_WEBVIEW_EVAL_ERROR;
        return;
    }
    jsr = p_webkit_web_view_run_javascript_finish(w->view, res, &err);
    if (err != NULL) {
        p_g_error_free(err);
        w->eval_result[0] = '\0';
        w->eval_state = ALYA_WEBVIEW_EVAL_ERROR;
        return;
    }
    if (jsr == NULL) {
        w->eval_result[0] = '\0';
        w->eval_state = ALYA_WEBVIEW_EVAL_ERROR;
        return;
    }
    jsv = p_webkit_javascript_result_get_js_value(jsr);
    if (jsv == NULL) {
        w->eval_result[0] = '\0';
        w->eval_state = ALYA_WEBVIEW_EVAL_ERROR;
        return;
    }
    s = p_jsc_value_to_string(jsv);
    if (s != NULL) {
        wv_copy(w->eval_result, sizeof(w->eval_result), s);
        p_g_free(s);
        w->eval_state = ALYA_WEBVIEW_EVAL_READY;
    } else {
        w->eval_result[0] = '\0';
        w->eval_state = ALYA_WEBVIEW_EVAL_ERROR;
    }
    // Note: WebKitJavascriptResult is transfer-full; the floating result
    // wrapper is owned by the async machinery and must not be freed here.
}

/* ---------------- public contract ---------------- */

int alya_webview_backend_id(void) {
    return 3;
}

const char *alya_webview_backend_name(void) {
    return "linux";
}

int alya_webview_open_external(const char *url) {
    pid_t first;
    // Double fork so no zombie is left behind and no GTK is needed:
    // works even when the engine libraries are absent. Requires the
    // ubiquitous xdg-open helper on desktop systems.
    if (url == NULL || url[0] == '\0') {
        return 0;
    }
    first = fork();
    if (first < 0) {
        return 0;
    }
    if (first == 0) {
        pid_t second = fork();
        if (second < 0) {
            _exit(127);
        }
        if (second == 0) {
            execlp("xdg-open", "xdg-open", url, (char *)NULL);
            _exit(127);
        }
        _exit(0);
    }
    // Reap the intermediate child; the grandchild is adopted by init.
    while (waitpid(first, NULL, 0) < 0) {
    }
    return 1;
}

alya_webview_t *alya_webview_create(const char *title, int width,
                                    int height) {
    alya_webview_t *w;
    void *ucm;
    void *settings;

    if (width <= 0) {
        width = 800;
    }
    if (height <= 0) {
        height = 600;
    }
    if (title == NULL || title[0] == '\0') {
        title = "Alya";
    }
    if (!wv_load_all()) {
        return NULL; // no GTK/WebKit on this machine
    }
    if (!p_gtk_init_check(NULL, NULL)) {
        return NULL; // headless: no display
    }

    w = (alya_webview_t *)calloc(1, sizeof(*w));
    if (w == NULL) {
        return NULL;
    }
    w->open = 1;
    w->width = width;
    w->height = height;
    wv_copy(w->title, sizeof(w->title), title);

    w->win = p_gtk_window_new(0); // GTK_WINDOW_TOPLEVEL
    if (w->win == NULL) {
        free(w);
        return NULL;
    }
    p_gtk_window_set_title(w->win, title);
    p_gtk_window_set_default_size(w->win, width, height);

    w->view = (GtkWidget *)p_webkit_web_view_new();
    if (w->view == NULL) {
        p_gtk_widget_destroy(w->win);
        free(w);
        return NULL;
    }
    p_gtk_container_add(w->win, w->view);

    settings = p_webkit_web_view_get_settings(w->view);
    if (settings != NULL) {
        p_webkit_settings_set_enable_javascript(settings, 1);
    }

    ucm = p_webkit_web_view_get_user_content_manager(w->view);
    if (ucm != NULL) {
        p_webkit_user_content_manager_register_script_message_handler(ucm,
                                                                      "alya");
    }

    p_g_signal_connect_data(w->view, "load-changed",
                            (void *)wv_on_load_changed, (void *)w, NULL, 0);
    p_g_signal_connect_data(w->view, "notify::title", (void *)wv_on_title,
                            (void *)w, NULL, 0);
    p_g_signal_connect_data(ucm, "script-message-received::alya",
                            (void *)wv_on_script_message, (void *)w, NULL,
                            0);
    p_g_signal_connect_data(w->win, "destroy", (void *)wv_on_destroy,
                            (void *)w, NULL, 0);

    if (p_webkit_web_view_evaluate_javascript != NULL &&
        p_webkit_web_view_evaluate_javascript_finish != NULL) {
        w->api = WV_API_41;
    } else if (p_webkit_web_view_run_javascript != NULL &&
               p_webkit_web_view_run_javascript_finish != NULL) {
        w->api = WV_API_40;
    } else {
        w->api = WV_API_NONE;
    }

    w->ready = 1;
    return w;
}

void alya_webview_destroy(alya_webview_t *w) {
    if (w == NULL) {
        return;
    }
    if (w->win != NULL) {
        p_gtk_widget_destroy(w->win);
        w->win = NULL;
        w->view = NULL;
    }
    w->open = 0;
    w->ready = 0;
    free(w);
}

void alya_webview_show(alya_webview_t *w) {
    if (w == NULL || w->win == NULL) {
        return;
    }
    p_gtk_widget_show_all(w->win);
}

void alya_webview_hide(alya_webview_t *w) {
    if (w == NULL || w->win == NULL) {
        return;
    }
    p_gtk_widget_hide(w->win);
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
    if (w == NULL) {
        return;
    }
    w->open = 0;
    wv_push(w, ALYA_WEBVIEW_EVENT_CLOSE);
    if (w->win != NULL) {
        p_gtk_widget_destroy(w->win);
        w->win = NULL;
        w->view = NULL;
    }
}

void alya_webview_set_title(alya_webview_t *w, const char *title) {
    if (w == NULL || w->win == NULL) {
        return;
    }
    if (title == NULL) {
        title = "";
    }
    wv_copy(w->title, sizeof(w->title), title);
    p_gtk_window_set_title(w->win, title);
}

void alya_webview_set_size(alya_webview_t *w, int width, int height) {
    if (w == NULL || w->win == NULL) {
        return;
    }
    if (width <= 0 || height <= 0) {
        return;
    }
    w->width = width;
    w->height = height;
    p_gtk_window_resize(w->win, width, height);
    wv_push(w, ALYA_WEBVIEW_EVENT_RESIZE);
}

int alya_webview_navigate(alya_webview_t *w, const char *url) {
    if (w == NULL || w->view == NULL) {
        return 0;
    }
    if (url == NULL || url[0] == '\0') {
        return 0;
    }
    wv_copy(w->url, sizeof(w->url), url);
    // NAV_START also arrives via load-changed; the local push covers
    // engines that coalesce the first signal.
    wv_push(w, ALYA_WEBVIEW_EVENT_NAV_START);
    p_webkit_web_view_load_uri(w->view, url);
    return 1;
}

int alya_webview_load_html(alya_webview_t *w, const char *html) {
    if (w == NULL || w->view == NULL) {
        return 0;
    }
    if (html == NULL) {
        html = "";
    }
    wv_copy(w->url, sizeof(w->url), "about:blank");
    wv_push(w, ALYA_WEBVIEW_EVENT_NAV_START);
    p_webkit_web_view_load_html(w->view, html, NULL);
    return 1;
}

int alya_webview_reload(alya_webview_t *w) {
    if (w == NULL || w->view == NULL) {
        return 0;
    }
    p_webkit_web_view_reload(w->view);
    return 1;
}

int alya_webview_go_back(alya_webview_t *w) {
    if (w == NULL || w->view == NULL) {
        return 0;
    }
    p_webkit_web_view_go_back(w->view);
    return 1;
}

int alya_webview_go_forward(alya_webview_t *w) {
    if (w == NULL || w->view == NULL) {
        return 0;
    }
    p_webkit_web_view_go_forward(w->view);
    return 1;
}

int alya_webview_can_back(alya_webview_t *w) {
    if (w == NULL || w->view == NULL) {
        return 0;
    }
    return p_webkit_web_view_can_go_back(w->view) ? 1 : 0;
}

int alya_webview_can_forward(alya_webview_t *w) {
    if (w == NULL || w->view == NULL) {
        return 0;
    }
    return p_webkit_web_view_can_go_forward(w->view) ? 1 : 0;
}

int alya_webview_eval(alya_webview_t *w, const char *js) {
    if (w == NULL || w->view == NULL) {
        return 0;
    }
    if (js == NULL || js[0] == '\0') {
        return 0;
    }
    w->eval_state = ALYA_WEBVIEW_EVAL_PENDING;
    w->eval_result[0] = '\0';
    if (w->api == WV_API_41) {
        p_webkit_web_view_evaluate_javascript(w->view, js, -1, NULL, NULL,
                                              NULL, NULL, wv_on_eval_41,
                                              (void *)w);
        return 1;
    }
    if (w->api == WV_API_40) {
        p_webkit_web_view_run_javascript(w->view, js, NULL, wv_on_eval_40,
                                         (void *)w);
        return 1;
    }
    w->eval_state = ALYA_WEBVIEW_EVAL_ERROR;
    return 0;
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
    if (w->api == WV_API_41) {
        p_webkit_web_view_evaluate_javascript(w->view, buf, -1, NULL, NULL,
                                              NULL, NULL, NULL, NULL);
        return 1;
    }
    if (w->api == WV_API_40) {
        p_webkit_web_view_run_javascript(w->view, buf, NULL, NULL, NULL);
        return 1;
    }
    return 0;
}

int alya_webview_set_devtools(alya_webview_t *w, int enabled) {
    void *settings;
    if (w == NULL || w->view == NULL) {
        return 0;
    }
    settings = p_webkit_web_view_get_settings(w->view);
    if (settings == NULL) {
        return 0;
    }
    p_webkit_settings_set_enable_developer_extras(settings,
                                                  enabled ? 1 : 0);
    return 1;
}

int alya_webview_set_js(alya_webview_t *w, int enabled) {
    void *settings;
    if (w == NULL || w->view == NULL) {
        return 0;
    }
    settings = p_webkit_web_view_get_settings(w->view);
    if (settings == NULL) {
        return 0;
    }
    p_webkit_settings_set_enable_javascript(settings, enabled ? 1 : 0);
    return 1;
}

int alya_webview_set_user_agent(alya_webview_t *w, const char *ua) {
    void *settings;
    if (w == NULL || w->view == NULL) {
        return 0;
    }
    if (ua == NULL) {
        ua = "";
    }
    settings = p_webkit_web_view_get_settings(w->view);
    if (settings == NULL) {
        return 0;
    }
    p_webkit_settings_set_user_agent(settings, ua);
    return 1;
}

int alya_webview_poll(alya_webview_t *w) {
    int kind;
    int guard;
    if (w == NULL) {
        return ALYA_WEBVIEW_EVENT_NONE;
    }
    guard = 0;
    while (p_gtk_events_pending() && guard < 64) {
        p_gtk_main_iteration_do(0);
        guard++;
    }
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
