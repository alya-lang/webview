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
    int allow_menu;  // native context menu policy (default 1)
    int block_keys;  // shortcut-blocking user script installed
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
WV_DECL(void *, webkit_web_view_new_with_context, void *ctx);
WV_DECL(void *, webkit_web_context_new_ephemeral, void);
WV_DECL(void *, webkit_web_context_new_with_website_data_manager,
        void *manager);
WV_DECL(void *, webkit_website_data_manager_new, const char *first, ...);
WV_DECL(double, webkit_web_view_get_zoom_level, GtkWidget *v);
WV_DECL(void, webkit_web_view_set_zoom_level, GtkWidget *v, double z);
WV_DECL(void *, gtk_widget_get_window, GtkWidget *w);
WV_DECL(void, gdk_event_put, void *ev);
WV_DECL(void, gdk_event_free, void *ev);
WV_DECL(unsigned int, gdk_unicode_to_keyval, unsigned int wc);
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
WV_DECL(unsigned int, webkit_get_major_version, void);
WV_DECL(unsigned int, webkit_get_minor_version, void);
WV_DECL(unsigned int, webkit_get_micro_version, void);
WV_DECL(void, webkit_web_view_set_background_color, GtkWidget *v,
        void *rgba);
WV_DECL(void, webkit_settings_set_auto_load_images, void *s, int v);
WV_DECL(void, webkit_settings_set_enable_webgl, void *s, int v);
WV_DECL(void, webkit_settings_set_default_charset, void *s,
        const char *cs);
WV_DECL(void *, webkit_user_script_new, const char *source, int frames,
        int injection_time, const char **allow_list,
        const char **block_list);
WV_DECL(void, webkit_user_content_manager_add_script, void *m, void *s);
WV_DECL(void, webkit_user_content_manager_remove_all_scripts, void *m);
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
    // Launch/input/zoom surface: hard requirements.
    WV_LOAD(wv_h_webkit, webkit_web_view_new_with_context);
    WV_LOAD(wv_h_webkit, webkit_web_context_new_ephemeral);
    WV_LOAD(wv_h_webkit, webkit_web_context_new_with_website_data_manager);
    WV_LOAD(wv_h_webkit, webkit_website_data_manager_new);
    WV_LOAD(wv_h_webkit, webkit_web_view_get_zoom_level);
    WV_LOAD(wv_h_webkit, webkit_web_view_set_zoom_level);
    WV_LOAD(wv_h_gtk, gtk_widget_get_window);
    WV_LOAD(wv_h_gtk, gdk_event_put);
    WV_LOAD(wv_h_gtk, gdk_event_free);
    WV_LOAD(wv_h_gtk, gdk_unicode_to_keyval);
    WV_LOAD(wv_h_webkit, webkit_get_major_version);
    WV_LOAD(wv_h_webkit, webkit_get_minor_version);
    WV_LOAD(wv_h_webkit, webkit_get_micro_version);
    WV_LOAD(wv_h_webkit, webkit_web_view_set_background_color);
    WV_LOAD(wv_h_webkit, webkit_settings_set_auto_load_images);
    WV_LOAD(wv_h_webkit, webkit_settings_set_enable_webgl);
    WV_LOAD(wv_h_webkit, webkit_settings_set_default_charset);
    WV_LOAD(wv_h_webkit, webkit_user_script_new);
    WV_LOAD(wv_h_webkit, webkit_user_content_manager_add_script);
    WV_LOAD(wv_h_webkit, webkit_user_content_manager_remove_all_scripts);

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

// Returning nonzero from "context-menu" suppresses the native menu.
static int wv_on_context_menu(GtkWidget *view, void *menu, void *event,
                              void *hit, void *data) {
    alya_webview_t *w = (alya_webview_t *)data;
    (void)view;
    (void)menu;
    (void)event;
    (void)hit;
    if (w != NULL && !w->allow_menu) {
        return 1;
    }
    return 0;
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

const char *alya_webview_engine_version(void) {
    static char cached[64];
    static int probed = 0;
    if (probed) {
        return cached;
    }
    probed = 1;
    cached[0] = '\0';
    if (wv_load_all()) {
        snprintf(cached, sizeof(cached), "%u.%u.%u",
                 p_webkit_get_major_version(), p_webkit_get_minor_version(),
                 p_webkit_get_micro_version());
    }
    return cached;
}

/* Launch configuration (process-wide, consumed by create below). */
static char wv_g_data_dir[1024];

void alya_webview_set_data_dir(const char *path) {
    if (path == NULL || path[0] == '\0') {
        wv_g_data_dir[0] = '\0';
        return;
    }
    strncpy(wv_g_data_dir, path, sizeof(wv_g_data_dir) - 1);
    wv_g_data_dir[sizeof(wv_g_data_dir) - 1] = '\0';
}

void alya_webview_set_extra_args(const char *args) {
    // No generic switch channel for WebKitGTK contexts in v1;
    // WEBKIT_* tuning stays in the user's shell environment.
    (void)args;
}

static GtkWidget *wv_new_view(int priv) {
    void *ctx = NULL;
    void *mgr = NULL;
    GtkWidget *v;
    if (priv) {
        ctx = p_webkit_web_context_new_ephemeral();
        if (ctx != NULL) {
            v = (GtkWidget *)p_webkit_web_view_new_with_context(ctx);
            if (v != NULL) {
                return v;
            }
        }
        // Fall through to the default view when ephemeral is missing.
    } else if (wv_g_data_dir[0] != '\0') {
        char ddata[1152];
        char dcache[1152];
        snprintf(ddata, sizeof(ddata), "%s/data", wv_g_data_dir);
        snprintf(dcache, sizeof(dcache), "%s/cache", wv_g_data_dir);
        mgr = p_webkit_website_data_manager_new("base-data-directory",
                                                ddata,
                                                "base-cache-directory",
                                                dcache, NULL);
        if (mgr != NULL) {
            ctx = p_webkit_web_context_new_with_website_data_manager(mgr);
            if (ctx != NULL) {
                v = (GtkWidget *)p_webkit_web_view_new_with_context(ctx);
                if (v != NULL) {
                    return v;
                }
            }
        }
        // Fall through to the default view when custom dirs fail.
    }
    return (GtkWidget *)p_webkit_web_view_new();
}

static alya_webview_t *wv_create_inner(const char *title, int width,
                                       int height, int priv);

alya_webview_t *alya_webview_create_private(const char *title, int width,
                                            int height) {
    return wv_create_inner(title, width, height, 1);
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
    return wv_create_inner(title, width, height, 0);
}

static alya_webview_t *wv_create_inner(const char *title, int width,
                                       int height, int priv) {
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
    w->allow_menu = 1;
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

    w->view = wv_new_view(priv);
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
    p_g_signal_connect_data(w->view, "context-menu",
                            (void *)wv_on_context_menu, (void *)w, NULL,
                            0);

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

/* Synthetic input via hand-built GdkEvents (GTK3 LP64 ABI).
 * Events target our own widget through gdk_event_put: no global
 * side effects, no extra permissions. Coordinates are window client
 * pixels, origin top-left. */

// GdkEventType (stable since GTK2).
#define WV_GDK_MOTION 3
#define WV_GDK_PRESS 4
#define WV_GDK_RELEASE 7
#define WV_GDK_KEY_PRESS 8
#define WV_GDK_KEY_RELEASE 9
#define WV_GDK_SCROLL 31
#define WV_GDK_SCROLL_SMOOTH 4

typedef struct {
    int type;
    void *window;
    signed char send_event;
    unsigned int time;
    double x;
    double y;
    double *axes;
    unsigned int state;
    int is_hint;
    void *device;
    double x_root;
    double y_root;
} wv_gdk_motion_t;

typedef struct {
    int type;
    void *window;
    signed char send_event;
    unsigned int time;
    double x;
    double y;
    double *axes;
    unsigned int state;
    unsigned int button;
    void *device;
    double x_root;
    double y_root;
} wv_gdk_button_t;

typedef struct {
    int type;
    void *window;
    signed char send_event;
    unsigned int time;
    double x;
    double y;
    double *axes;
    unsigned int state;
    int direction;
    double x_root;
    double y_root;
    double delta_x;
    double delta_y;
    unsigned int is_stop;
    void *device;
} wv_gdk_scroll_t;

typedef struct {
    int type;
    void *window;
    signed char send_event;
    unsigned int time;
    unsigned int state;
    unsigned int keyval;
    int length;
    char *string;
    unsigned short hardware_keycode;
    unsigned char group;
    unsigned int is_modifier;
} wv_gdk_key_t;

static void *wv_widget_window(alya_webview_t *w) {
    void *win;
    if (w == NULL || w->view == NULL) {
        return NULL;
    }
    win = p_gtk_widget_get_window(w->view);
    return win;
}

static int wv_put_event(alya_webview_t *w, void *ev) {
    if (w == NULL || ev == NULL) {
        return 0;
    }
    p_gdk_event_put(ev);
    p_gdk_event_free(ev);
    return 1;
}

int alya_webview_mouse_move(alya_webview_t *w, int x, int y) {
    wv_gdk_motion_t *ev;
    if (wv_widget_window(w) == NULL) {
        return 0;
    }
    ev = (wv_gdk_motion_t *)calloc(1, sizeof(*ev));
    if (ev == NULL) {
        return 0;
    }
    ev->type = WV_GDK_MOTION;
    ev->window = wv_widget_window(w);
    ev->send_event = 1;
    ev->time = 0; // GDK_CURRENT_TIME
    ev->x = (double)x;
    ev->y = (double)y;
    return wv_put_event(w, ev);
}

static int wv_mouse_btn(alya_webview_t *w, int button, int down) {
    static const unsigned int gdk_btn[] = {1, 3, 2};
    wv_gdk_button_t *ev;
    if (wv_widget_window(w) == NULL) {
        return 0;
    }
    if (button < 0 || button > 2) {
        return 0;
    }
    ev = (wv_gdk_button_t *)calloc(1, sizeof(*ev));
    if (ev == NULL) {
        return 0;
    }
    ev->type = down ? WV_GDK_PRESS : WV_GDK_RELEASE;
    ev->window = wv_widget_window(w);
    ev->send_event = 1;
    ev->time = 0;
    ev->button = gdk_btn[button];
    return wv_put_event(w, ev);
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
    wv_gdk_scroll_t *ev;
    if (wv_widget_window(w) == NULL) {
        return 0;
    }
    if (dx == 0 && dy == 0) {
        return 0;
    }
    ev = (wv_gdk_scroll_t *)calloc(1, sizeof(*ev));
    if (ev == NULL) {
        return 0;
    }
    ev->type = WV_GDK_SCROLL;
    ev->window = wv_widget_window(w);
    ev->send_event = 1;
    ev->time = 0;
    ev->direction = WV_GDK_SCROLL_SMOOTH;
    ev->delta_x = (double)dx;
    ev->delta_y = (double)dy;
    return wv_put_event(w, ev);
}

static int wv_post_keyval(alya_webview_t *w, unsigned int keyval,
                          const char *text, int down) {
    wv_gdk_key_t *ev;
    if (wv_widget_window(w) == NULL) {
        return 0;
    }
    ev = (wv_gdk_key_t *)calloc(1, sizeof(*ev));
    if (ev == NULL) {
        return 0;
    }
    ev->type = down ? WV_GDK_KEY_PRESS : WV_GDK_KEY_RELEASE;
    ev->window = wv_widget_window(w);
    ev->send_event = 1;
    ev->time = 0;
    ev->state = 0;
    ev->keyval = keyval;
    if (text != NULL) {
        ev->length = (int)strlen(text);
        ev->string = (char *)text; // copied by gdk_event_put
    }
    ev->hardware_keycode = 0;
    ev->group = 0;
    ev->is_modifier = 0;
    return wv_put_event(w, ev);
}

int alya_webview_key_down(alya_webview_t *w, int code) {
    if (code <= 0) {
        return 0;
    }
    return wv_post_keyval(w, (unsigned int)code, NULL, 1);
}

int alya_webview_key_up(alya_webview_t *w, int code) {
    if (code <= 0) {
        return 0;
    }
    return wv_post_keyval(w, (unsigned int)code, NULL, 0);
}

int alya_webview_key_tap(alya_webview_t *w, int code) {
    int d;
    int u;
    if (w == NULL || code <= 0) {
        return 0;
    }
    d = wv_post_keyval(w, (unsigned int)code, NULL, 1);
    u = wv_post_keyval(w, (unsigned int)code, NULL, 0);
    return (d && u) ? 1 : 0;
}

static int wv_utf8_next(const unsigned char *p, unsigned int *cp) {
    if ((*p & 0x80) == 0) {
        *cp = *p;
        return 1;
    } else if ((*p & 0xE0) == 0xC0) {
        *cp = ((unsigned int)(p[0] & 0x1F) << 6) |
              (unsigned int)(p[1] & 0x3F);
        return 2;
    } else if ((*p & 0xF0) == 0xE0) {
        *cp = ((unsigned int)(p[0] & 0x0F) << 12) |
              ((unsigned int)(p[1] & 0x3F) << 6) |
              (unsigned int)(p[2] & 0x3F);
        return 3;
    } else if ((*p & 0xF8) == 0xF0) {
        *cp = ((unsigned int)(p[0] & 0x07) << 18) |
              ((unsigned int)(p[1] & 0x3F) << 12) |
              ((unsigned int)(p[2] & 0x3F) << 6) |
              (unsigned int)(p[3] & 0x3F);
        return 4;
    }
    *cp = *p;
    return 1;
}

int alya_webview_key_text(alya_webview_t *w, const char *text) {
    const unsigned char *p;
    int ok = 0;
    if (w == NULL || text == NULL || text[0] == '\0') {
        return 0;
    }
    if (wv_widget_window(w) == NULL) {
        return 0;
    }
    p = (const unsigned char *)text;
    while (*p != '\0') {
        unsigned int cp;
        unsigned int kv;
        char one[5];
        int n = wv_utf8_next(p, &cp);
        if (n > 4) {
            n = 1;
        }
        memcpy(one, p, (size_t)n);
        one[n] = '\0';
        kv = p_gdk_unicode_to_keyval(cp);
        if (wv_post_keyval(w, kv, one, 1)) {
            ok = 1;
        }
        wv_post_keyval(w, kv, one, 0);
        p += n;
    }
    return ok;
}

int alya_webview_key_code(const char *name) {
    static const struct {
        const char *name;
        int code;
    } map[] = {{"Enter", 0xFF0D},    {"Escape", 0xFF1B},
               {"Tab", 0xFF09},      {"Backspace", 0xFF08},
               {"Delete", 0xFFFF},   {"Left", 0xFF51},
               {"Up", 0xFF52},       {"Right", 0xFF53},
               {"Down", 0xFF54},     {"Home", 0xFF50},
               {"End", 0xFF57},      {"PageUp", 0xFF55},
               {"PageDown", 0xFF56}, {NULL, -1}};
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
    if (w == NULL || w->view == NULL) {
        return 0;
    }
    if (factor < 0.25 || factor > 5.0) {
        return 0;
    }
    p_webkit_web_view_set_zoom_level(w->view, factor);
    return 1;
}

double alya_webview_get_zoom(alya_webview_t *w) {
    if (w == NULL || w->view == NULL) {
        return 0.0;
    }
    return p_webkit_web_view_get_zoom_level(w->view);
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
    void *ucm;
    if (w == NULL || w->view == NULL) {
        return;
    }
    ucm = p_webkit_web_view_get_user_content_manager(w->view);
    if (ucm == NULL) {
        return;
    }
    p_webkit_user_content_manager_remove_all_scripts(ucm);
    if (!w->allow_menu) {
        // Belt and suspenders next to the native signal suppressor.
        void *s = p_webkit_user_script_new(wv_script_nomenu, 0, 0, NULL,
                                           NULL);
        if (s != NULL) {
            p_webkit_user_content_manager_add_script(ucm, s);
        }
    }
    if (w->block_keys) {
        void *s = p_webkit_user_script_new(wv_script_shortcuts, 0, 0,
                                           NULL, NULL);
        if (s != NULL) {
            p_webkit_user_content_manager_add_script(ucm, s);
        }
    }
}

typedef struct {
    double red;
    double green;
    double blue;
    double alpha;
} wv_rgba_t;

static int wv_clamp255(int v) {
    if (v < 0) {
        return 0;
    }
    if (v > 255) {
        return 255;
    }
    return v;
}

int alya_webview_set_background(alya_webview_t *w, int r, int g, int b,
                                int a) {
    wv_rgba_t rgba;
    if (w == NULL || w->view == NULL) {
        return 0;
    }
    rgba.red = (double)wv_clamp255(r) / 255.0;
    rgba.green = (double)wv_clamp255(g) / 255.0;
    rgba.blue = (double)wv_clamp255(b) / 255.0;
    rgba.alpha = (double)wv_clamp255(a) / 255.0;
    p_webkit_web_view_set_background_color(w->view, &rgba);
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

static void *wv_view_settings(alya_webview_t *w) {
    if (w == NULL || w->view == NULL) {
        return NULL;
    }
    return p_webkit_web_view_get_settings(w->view);
}

int alya_webview_set_images(alya_webview_t *w, int enabled) {
    void *s = wv_view_settings(w);
    if (s == NULL) {
        return 0;
    }
    p_webkit_settings_set_auto_load_images(s, enabled ? 1 : 0);
    return 1;
}

int alya_webview_set_webgl(alya_webview_t *w, int enabled) {
    void *s = wv_view_settings(w);
    if (s == NULL) {
        return 0;
    }
    p_webkit_settings_set_enable_webgl(s, enabled ? 1 : 0);
    return 1;
}

int alya_webview_set_charset(alya_webview_t *w, const char *cs) {
    void *s = wv_view_settings(w);
    if (s == NULL) {
        return 0;
    }
    if (cs == NULL || cs[0] == '\0') {
        cs = "UTF-8";
    }
    p_webkit_settings_set_default_charset(s, cs);
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
