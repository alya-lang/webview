// Windows backend for the Alya webview package: WebView2 (Edge Chromium).
//
// Pure C, no C++ runtime, no WebView2 SDK headers, no link-time dependency
// on any WebView2 loader. The loader DLL is resolved at runtime (app-local
// WebView2Loader.dll first, then the evergreen EdgeWebView install), so the
// package builds on plain MinGW and create() degrades gracefully when no
// runtime is present: the window and event queue keep working, only the
// engine calls report "not ready" (headless CI safe).
//
// COM vtables below mirror WebView2 SDK 1.0 IDL slot order. If a future
// SDK renumbers a method, only the wv_core_*/wv_ctl_*/wv_env_* wrappers
// change; the Alya-facing contract in webview.h is unaffected.
//
// Threading: everything runs on the calling thread. COM is initialized
// apartment-threaded and WebView2 completion callbacks fire while poll()
// pumps messages, so no worker threads are ever created.

#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "webview.h"

#define WV_MAX_EVENTS 64
#define WV_CREATE_TIMEOUT_MS 20000

typedef struct {
    int kind;
    int w;
    int h;
    char url[ALYA_WEBVIEW_URL_CAP];
    char text[ALYA_WEBVIEW_TEXT_CAP];
} wv_event_t;

struct alya_webview {
    HWND hwnd;
    HWND child; // WebView2 content child (resolved lazily, may be NULL)
    int32_t open;
    int32_t ready;
    int32_t width;
    int32_t height;
    int32_t mouse_x; // last injected cursor position (client pixels)
    int32_t mouse_y;
    int32_t eval_state;
    char url[ALYA_WEBVIEW_URL_CAP];
    char title[ALYA_WEBVIEW_TEXT_CAP];
    char message[ALYA_WEBVIEW_TEXT_CAP];
    char eval_result[ALYA_WEBVIEW_TEXT_CAP];
    int32_t head;
    int32_t tail;
    wv_event_t queue[WV_MAX_EVENTS];
    // COM state (all owned on the creating thread).
    void *env;   // ICoreWebView2Environment*
    void *ctl;   // ICoreWebView2Controller*
    void *core;  // ICoreWebView2*
    HANDLE done; // signalled when controller creation finished (ok or not)
};

/* ---------------- small utilities ---------------- */

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

static wchar_t *wv_utf8_to_wide(const char *s) {
    int n;
    wchar_t *out;
    if (s == NULL) {
        s = "";
    }
    n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0) {
        return NULL;
    }
    out = (wchar_t *)malloc((size_t)n * sizeof(wchar_t));
    if (out == NULL) {
        return NULL;
    }
    MultiByteToWideChar(CP_UTF8, 0, s, -1, out, n);
    return out;
}

static void wv_wide_to_utf8_into(char *dst, size_t cap, const wchar_t *s) {
    int n;
    if (cap == 0) {
        return;
    }
    if (s == NULL) {
        dst[0] = '\0';
        return;
    }
    n = WideCharToMultiByte(CP_UTF8, 0, s, -1, dst, (int)cap, NULL, NULL);
    if (n <= 0) {
        dst[0] = '\0';
    }
}

static void wv_push(alya_webview_t *w, int kind) {
    int32_t next;
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

/* ---------------- minimal COM plumbing ----------------
 * Every callback object is one wv_cb_t: a 4-slot vtable
 * {QueryInterface, AddRef, Release, Invoke} plus an owner pointer.
 * Invoke signatures differ per callback kind, but the function pointer
 * always sits at the same slot, so one struct serves all kinds.
 * (HRESULT, REFIID come from windows.h / objbase.h; IUnknown's own
 * vtable is reused for AddRef/Release, so it is not redeclared here.) */

typedef HRESULT(STDMETHODCALLTYPE *wv_invoke_fn)(void);

typedef struct wv_cb {
    wv_invoke_fn *slots;
    LONG refs;
    alya_webview_t *w;
} wv_cb_t;

static HRESULT STDMETHODCALLTYPE wv_cb_qi(void *self, REFIID riid,
                                          void **out) {
    (void)riid;
    if (out != NULL) {
        *out = self;
    }
    return S_OK;
}

static unsigned long STDMETHODCALLTYPE wv_cb_add(void *self) {
    wv_cb_t *c = (wv_cb_t *)self;
    return (unsigned long)InterlockedIncrement(&c->refs);
}

static unsigned long STDMETHODCALLTYPE wv_cb_rel(void *self) {
    wv_cb_t *c = (wv_cb_t *)self;
    LONG n = InterlockedDecrement(&c->refs);
    if (n == 0) {
        free(c);
    }
    return (unsigned long)(n < 0 ? 0 : n);
}

static void wv_iface_release(void *iface) {
    if (iface != NULL) {
        IUnknown *unk = (IUnknown *)iface;
        unk->lpVtbl->Release(unk);
    }
}

static void wv_iface_addref(void *iface) {
    if (iface != NULL) {
        IUnknown *unk = (IUnknown *)iface;
        unk->lpVtbl->AddRef(unk);
    }
}

/* Raw vtable call: slot index per WebView2 SDK 1.0 IDL order.
 * The callee takes fewer parameters; extra NULL args are harmless
 * (stdcall callee cleans the stack). */
static HRESULT wv_call(void *iface, int slot, ...) {
    void **vt;
    HRESULT(STDMETHODCALLTYPE *fn)(void *, void *, void *, void *, void *);
    va_list ap;
    void *a0, *a1, *a2, *a3;
    HRESULT hr;
    if (iface == NULL) {
        return E_POINTER;
    }
    vt = *(void ***)iface;
    fn = (HRESULT(STDMETHODCALLTYPE *)(void *, void *, void *, void *,
                                      void *))vt[slot];
    va_start(ap, slot);
    a0 = va_arg(ap, void *);
    a1 = va_arg(ap, void *);
    a2 = va_arg(ap, void *);
    a3 = va_arg(ap, void *);
    va_end(ap);
    hr = fn(iface, a0, a1, a2, a3);
    return hr;
}

/* IDL slot map (WebView2 SDK 1.0, stable since first release):
 * ICoreWebView2: get_Settings=3 get_Source=4 Navigate=5 NavigateToString=6
 *   add_NavigationCompleted=15 ExecuteScript=29 Reload=31
 *   PostWebMessageAsString=32 get_CanGoBack=36 get_CanGoForward=37
 *   GoBack=38 GoForward=39 get_DocumentTitle=42 add_WebMessageReceived=47
 *   add_DocumentTitleChanged=49
 * ICoreWebView2Controller: put_IsVisible=4 put_Bounds=6 Close=24
 *   get_CoreWebView2=25
 * ICoreWebView2Environment: CreateCoreWebView2Controller=3
 * ICoreWebView2Settings: put_AreDevToolsEnabled=11 (verified safe on
 *   evergreen WebView2: accepts a BOOL, returns normally). The script
 *   toggle slot is deliberately NOT called: probing showed the assumed
 *   slot faults on current runtimes, so set_js reports unsupported until
 *   the map is re-verified against real SDK headers (v1.1 task).
 *   Rule for this file: never call a slot that has not been observed to
 *   return on a real runtime; report 0 instead of risking a fault.
 * Arg ifaces: NavigationCompleted.get_IsSuccess=3
 *   WebMessageReceived.TryGetWebMessageAsString=4 */

static HRESULT wv_core_navigate(void *core, LPCWSTR url) {
    return wv_call(core, 5, (void *)url, NULL, NULL, NULL);
}

static HRESULT wv_core_navigate_to_string(void *core, LPCWSTR html) {
    return wv_call(core, 6, (void *)html, NULL, NULL, NULL);
}

static HRESULT wv_core_reload(void *core) {
    return wv_call(core, 31, NULL, NULL, NULL, NULL);
}

static HRESULT wv_core_execute_script(void *core, LPCWSTR js, void *h) {
    return wv_call(core, 29, (void *)js, h, NULL, NULL);
}

static HRESULT wv_core_post_message(void *core, LPCWSTR json) {
    return wv_call(core, 32, (void *)json, NULL, NULL, NULL);
}

static HRESULT wv_core_get_source(void *core, LPWSTR *out) {
    return wv_call(core, 4, (void *)out, NULL, NULL, NULL);
}

static HRESULT wv_core_get_title(void *core, LPWSTR *out) {
    return wv_call(core, 42, (void *)out, NULL, NULL, NULL);
}

static HRESULT wv_core_get_settings(void *core, void **out) {
    return wv_call(core, 3, (void *)out, NULL, NULL, NULL);
}

static HRESULT wv_core_can_back(void *core, int *out) {
    return wv_call(core, 36, (void *)out, NULL, NULL, NULL);
}

static HRESULT wv_core_can_fwd(void *core, int *out) {
    return wv_call(core, 37, (void *)out, NULL, NULL, NULL);
}

static HRESULT wv_core_go_back(void *core) {
    return wv_call(core, 38, NULL, NULL, NULL, NULL);
}

static HRESULT wv_core_go_fwd(void *core) {
    return wv_call(core, 39, NULL, NULL, NULL, NULL);
}

static HRESULT wv_core_add_completed(void *core, void *h, void *token) {
    return wv_call(core, 15, h, token, NULL, NULL);
}

static HRESULT wv_core_add_message(void *core, void *h, void *token) {
    return wv_call(core, 47, h, token, NULL, NULL);
}

static HRESULT wv_core_add_title(void *core, void *h, void *token) {
    return wv_call(core, 49, h, token, NULL, NULL);
}

static HRESULT wv_ctl_set_visible(void *ctl, int v) {
    return wv_call(ctl, 4, (void *)(INT_PTR)v, NULL, NULL, NULL);
}

static HRESULT wv_ctl_set_bounds(void *ctl, RECT *rc) {
    return wv_call(ctl, 6, (void *)rc, NULL, NULL, NULL);
}

static HRESULT wv_ctl_close(void *ctl) {
    return wv_call(ctl, 24, NULL, NULL, NULL, NULL);
}

static HRESULT wv_ctl_get_core(void *ctl, void **out) {
    return wv_call(ctl, 25, (void *)out, NULL, NULL, NULL);
}

static HRESULT wv_env_create_ctl(void *env, HWND hwnd, void *h) {
    return wv_call(env, 3, (void *)hwnd, h, NULL, NULL);
}

static HRESULT wv_settings_put_devtools(void *s, int v) {
    return wv_call(s, 11, (void *)(INT_PTR)v, NULL, NULL, NULL);
}

static HRESULT wv_nav_done_ok(void *args, int *out) {
    return wv_call(args, 3, (void *)out, NULL, NULL, NULL);
}

static HRESULT wv_msg_text(void *args, LPWSTR *out) {
    return wv_call(args, 4, (void *)out, NULL, NULL, NULL);
}

static void wv_free_str(LPWSTR s) {
    if (s != NULL) {
        CoTaskMemFree(s);
    }
}

/* Subscription callbacks (always run on the creating/UI thread). */

static HRESULT STDMETHODCALLTYPE wv_on_nav_done(void *self, void *sender,
                                               void *args) {
    wv_cb_t *c = (wv_cb_t *)self;
    alya_webview_t *w = c->w;
    int ok = 0;
    LPWSTR src = NULL;
    (void)sender;
    if (args != NULL) {
        wv_nav_done_ok(args, &ok);
    }
    if (w != NULL) {
        if (w->core != NULL &&
            SUCCEEDED(wv_core_get_source(w->core, &src)) && src != NULL) {
            wv_wide_to_utf8_into(w->url, sizeof(w->url), src);
        }
        wv_free_str(src);
        wv_push(w, ALYA_WEBVIEW_EVENT_NAV_DONE);
        (void)ok;
    }
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE wv_on_message(void *self, void *sender,
                                               void *args) {
    wv_cb_t *c = (wv_cb_t *)self;
    alya_webview_t *w = c->w;
    LPWSTR msg = NULL;
    (void)sender;
    if (w != NULL && args != NULL &&
        SUCCEEDED(wv_msg_text(args, &msg)) && msg != NULL) {
        wv_wide_to_utf8_into(w->message, sizeof(w->message), msg);
        wv_push(w, ALYA_WEBVIEW_EVENT_MESSAGE);
    }
    wv_free_str(msg);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE wv_on_title(void *self, void *sender,
                                             void *args) {
    wv_cb_t *c = (wv_cb_t *)self;
    alya_webview_t *w = c->w;
    LPWSTR t = NULL;
    (void)sender;
    (void)args;
    if (w != NULL && w->core != NULL &&
        SUCCEEDED(wv_core_get_title(w->core, &t)) && t != NULL) {
        wv_wide_to_utf8_into(w->title, sizeof(w->title), t);
        wv_push(w, ALYA_WEBVIEW_EVENT_TITLE);
    }
    wv_free_str(t);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE wv_on_script(void *self, HRESULT err,
                                              LPCWSTR result_json) {
    wv_cb_t *c = (wv_cb_t *)self;
    alya_webview_t *w = c->w;
    if (w != NULL) {
        if (SUCCEEDED(err) && result_json != NULL) {
            wv_wide_to_utf8_into(w->eval_result, sizeof(w->eval_result),
                                 result_json);
            w->eval_state = ALYA_WEBVIEW_EVAL_READY;
        } else {
            w->eval_result[0] = '\0';
            w->eval_state = ALYA_WEBVIEW_EVAL_ERROR;
        }
    }
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE wv_on_ctl(void *self, HRESULT err,
                                           void *ctl) {
    wv_cb_t *c = (wv_cb_t *)self;
    alya_webview_t *w = c->w;
    if (w != NULL) {
        if (SUCCEEDED(err) && ctl != NULL) {
            wv_iface_addref(ctl);
            w->ctl = ctl;
        }
        SetEvent(w->done);
    }
    return S_OK;
}

/* Forward slot tables (defined below, used by the env callback). */
static wv_invoke_fn wv_slots_ctl[4];

static HRESULT STDMETHODCALLTYPE wv_on_env(void *self, HRESULT err,
                                           void *env) {
    wv_cb_t *c = (wv_cb_t *)self;
    alya_webview_t *w = c->w;
    wv_cb_t *ch;
    if (w == NULL) {
        return S_OK;
    }
    if (FAILED(err) || env == NULL) {
        SetEvent(w->done);
        return S_OK;
    }
    wv_iface_addref(env);
    w->env = env;
    ch = (wv_cb_t *)calloc(1, sizeof(*ch));
    if (ch == NULL) {
        SetEvent(w->done);
        return S_OK;
    }
    ch->slots = wv_slots_ctl;
    ch->refs = 1;
    ch->w = w;
    if (FAILED(wv_env_create_ctl(env, w->hwnd, (void *)ch))) {
        free(ch);
        SetEvent(w->done);
        return S_OK;
    }
    // WebView2 holds its own reference from here on.
    wv_cb_rel((void *)ch);
    return S_OK;
}

static wv_invoke_fn wv_slots_env[4] = {(wv_invoke_fn)wv_cb_qi,
                                       (wv_invoke_fn)wv_cb_add,
                                       (wv_invoke_fn)wv_cb_rel,
                                       (wv_invoke_fn)wv_on_env};
static wv_invoke_fn wv_slots_ctl[4] = {(wv_invoke_fn)wv_cb_qi,
                                       (wv_invoke_fn)wv_cb_add,
                                       (wv_invoke_fn)wv_cb_rel,
                                       (wv_invoke_fn)wv_on_ctl};
static wv_invoke_fn wv_slots_script[4] = {
    (wv_invoke_fn)wv_cb_qi, (wv_invoke_fn)wv_cb_add,
    (wv_invoke_fn)wv_cb_rel, (wv_invoke_fn)wv_on_script};
static wv_invoke_fn wv_slots_done[4] = {(wv_invoke_fn)wv_cb_qi,
                                        (wv_invoke_fn)wv_cb_add,
                                        (wv_invoke_fn)wv_cb_rel,
                                        (wv_invoke_fn)wv_on_nav_done};
static wv_invoke_fn wv_slots_msg[4] = {(wv_invoke_fn)wv_cb_qi,
                                       (wv_invoke_fn)wv_cb_add,
                                       (wv_invoke_fn)wv_cb_rel,
                                       (wv_invoke_fn)wv_on_message};
static wv_invoke_fn wv_slots_title[4] = {
    (wv_invoke_fn)wv_cb_qi, (wv_invoke_fn)wv_cb_add,
    (wv_invoke_fn)wv_cb_rel, (wv_invoke_fn)wv_on_title};

static wv_cb_t *wv_make_cb(alya_webview_t *w, wv_invoke_fn *slots) {
    wv_cb_t *c = (wv_cb_t *)calloc(1, sizeof(*c));
    if (c == NULL) {
        return NULL;
    }
    c->slots = slots;
    c->refs = 1;
    c->w = w;
    return c;
}

/* ---------------- loader discovery ---------------- */

typedef HRESULT(STDMETHODCALLTYPE *wv_create_env_fn)(
    LPCWSTR browser_folder, LPCWSTR user_data_folder, void *options,
    void *handler);

static HMODULE wv_try_eb_dir(const wchar_t *dir) {
    wchar_t path[MAX_PATH * 2];
    HMODULE m;
    if (dir == NULL) {
        return NULL;
    }
    _snwprintf(path, sizeof(path) / sizeof(path[0]),
               L"%s\\EBWebView\\x64\\EmbeddedBrowserWebView.dll", dir);
    path[(sizeof(path) / sizeof(path[0])) - 1] = L'\0';
    m = LoadLibraryW(path);
    if (m != NULL) {
        return m;
    }
    _snwprintf(path, sizeof(path) / sizeof(path[0]),
               L"%s\\EBWebView\\x86\\EmbeddedBrowserWebView.dll", dir);
    path[(sizeof(path) / sizeof(path[0])) - 1] = L'\0';
    return LoadLibraryW(path);
}

static HMODULE wv_scan_edge_dir(const wchar_t *progfiles) {
    wchar_t probe[MAX_PATH * 2];
    HANDLE fh;
    WIN32_FIND_DATAW fd;
    HMODULE m = NULL;
    _snwprintf(probe, sizeof(probe) / sizeof(probe[0]),
               L"%s\\Microsoft\\EdgeWebView\\Application\\*", progfiles);
    probe[(sizeof(probe) / sizeof(probe[0])) - 1] = L'\0';
    fh = FindFirstFileW(probe, &fd);
    if (fh == INVALID_HANDLE_VALUE) {
        return NULL;
    }
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
            fd.cFileName[0] != L'.') {
            _snwprintf(probe, sizeof(probe) / sizeof(probe[0]),
                       L"%s\\Microsoft\\EdgeWebView\\Application\\%s",
                       progfiles, fd.cFileName);
            probe[(sizeof(probe) / sizeof(probe[0])) - 1] = L'\0';
            m = wv_try_eb_dir(probe);
            if (m != NULL) {
                break;
            }
        }
    } while (FindNextFileW(fh, &fd));
    FindClose(fh);
    return m;
}

static HMODULE wv_find_loader(void) {
    HMODULE m = LoadLibraryW(L"WebView2Loader.dll");
    wchar_t base[MAX_PATH];
    DWORD n;
    if (m != NULL) {
        return m;
    }
    n = GetEnvironmentVariableW(L"ProgramFiles", base, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        m = wv_scan_edge_dir(base);
    }
    if (m != NULL) {
        return m;
    }
    n = GetEnvironmentVariableW(L"ProgramFiles(x86)", base, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        m = wv_scan_edge_dir(base);
    }
    return m;
}

/* ---------------- window plumbing ---------------- */

static const wchar_t *wv_class_name(void) {
    return L"AlyaWebviewWindow";
}

static void wv_update_bounds(alya_webview_t *w) {
    if (w != NULL && w->ctl != NULL) {
        RECT rc;
        rc.left = 0;
        rc.top = 0;
        rc.right = w->width;
        rc.bottom = w->height;
        wv_ctl_set_bounds(w->ctl, &rc);
    }
}

static LRESULT CALLBACK wv_wndproc(HWND hwnd, UINT msg, WPARAM wp,
                                   LPARAM lp) {
    alya_webview_t *w;
    if (msg == WM_CREATE) {
        CREATESTRUCTW *cs = (CREATESTRUCTW *)lp;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        return 0;
    }
    w = (alya_webview_t *)(void *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (w == NULL) {
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    switch (msg) {
    case WM_SIZE: {
        RECT rc;
        if (GetClientRect(hwnd, &rc)) {
            w->width = (int)(rc.right - rc.left);
            w->height = (int)(rc.bottom - rc.top);
            wv_update_bounds(w);
            wv_push(w, ALYA_WEBVIEW_EVENT_RESIZE);
        }
        return 0;
    }
    case WM_CLOSE:
        w->open = 0;
        wv_push(w, ALYA_WEBVIEW_EVENT_CLOSE);
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        w->open = 0;
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void wv_pump(DWORD timeout_ms) {
    MSG msg;
    DWORD start = GetTickCount();
    for (;;) {
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (timeout_ms == 0) {
            break;
        }
        if (GetTickCount() - start >= timeout_ms) {
            break;
        }
        MsgWaitForMultipleObjects(0, NULL, FALSE, 1, QS_ALLINPUT);
    }
}

static void wv_release_com(alya_webview_t *w) {
    wv_iface_release(w->core);
    w->core = NULL;
    wv_iface_release(w->ctl);
    w->ctl = NULL;
    wv_iface_release(w->env);
    w->env = NULL;
    w->ready = 0;
}

/* ---------------- public contract ---------------- */

int alya_webview_backend_id(void) {
    return 1;
}

const char *alya_webview_backend_name(void) {
    return "windows";
}

/* Launch configuration (process-wide, consumed by create below). */
static wchar_t wv_g_data_dir[MAX_PATH * 2];
static int wv_g_data_dir_set = 0;
static LONG wv_g_private_seq = 0;

void alya_webview_set_data_dir(const char *path) {
    wchar_t *w;
    if (path == NULL || path[0] == '\0') {
        wv_g_data_dir_set = 0;
        return;
    }
    w = wv_utf8_to_wide(path);
    if (w == NULL) {
        return;
    }
    wcsncpy(wv_g_data_dir, w, (sizeof(wv_g_data_dir) / sizeof(wchar_t)) - 1);
    wv_g_data_dir[(sizeof(wv_g_data_dir) / sizeof(wchar_t)) - 1] = L'\0';
    free(w);
    wv_g_data_dir_set = 1;
}

void alya_webview_set_extra_args(const char *args) {
    // Honored by the WebView2 loader as additional Chromium switches.
    // Must precede environment creation, i.e. set before open().
    wchar_t *w;
    if (args == NULL || args[0] == '\0') {
        SetEnvironmentVariableW(L"WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS",
                                NULL);
        return;
    }
    w = wv_utf8_to_wide(args);
    if (w == NULL) {
        return;
    }
    SetEnvironmentVariableW(L"WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS", w);
    free(w);
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

int alya_webview_open_external(const char *url) {
    wchar_t *u;
    HINSTANCE rc;
    if (url == NULL || url[0] == '\0') {
        return 0;
    }
    u = wv_utf8_to_wide(url);
    if (u == NULL) {
        return 0;
    }
    rc = ShellExecuteW(NULL, L"open", u, NULL, NULL, SW_SHOWNORMAL);
    free(u);
    return ((INT_PTR)rc > 32) ? 1 : 0;
}

static alya_webview_t *wv_create_inner(const char *title, int width,
                                              int height, int priv) {
    alya_webview_t *w;
    WNDCLASSW kc;
    wchar_t *wtitle = NULL;
    HWND hwnd = NULL;
    HMODULE loader = NULL;
    wv_create_env_fn create_env = NULL;
    wv_cb_t *eh = NULL;
    wchar_t data_dir[MAX_PATH * 2];
    wchar_t *wdata = NULL;
    wchar_t tmp[MAX_PATH];
    DWORD tn;
    DWORD waited = 0;
    DWORD wait;

    if (width <= 0) {
        width = 800;
    }
    if (height <= 0) {
        height = 600;
    }
    if (title == NULL || title[0] == '\0') {
        title = "Alya";
    }

    // Apartment-threaded COM for WebView2.
    {
        HRESULT hr = CoInitializeEx(NULL,
                                    COINIT_APARTMENTTHREADED |
                                        COINIT_DISABLE_OLE1DDE);
        if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
            return NULL;
        }
    }

    memset(&kc, 0, sizeof(kc));
    kc.lpfnWndProc = wv_wndproc;
    kc.hInstance = GetModuleHandleW(NULL);
    kc.lpszClassName = wv_class_name();
    kc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    RegisterClassW(&kc);

    w = (alya_webview_t *)calloc(1, sizeof(*w));
    if (w == NULL) {
        return NULL;
    }
    w->open = 1;
    w->width = width;
    w->height = height;
    wv_copy(w->title, sizeof(w->title), title);
    w->done = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (w->done == NULL) {
        free(w);
        return NULL;
    }

    wtitle = wv_utf8_to_wide(title);
    {
        RECT rc;
        int ww = width;
        int hh = height;
        rc.left = 0;
        rc.top = 0;
        rc.right = width;
        rc.bottom = height;
        if (AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE)) {
            ww = rc.right - rc.left;
            hh = rc.bottom - rc.top;
        }
        hwnd = CreateWindowExW(0, wv_class_name(),
                               wtitle != NULL ? wtitle : L"Alya",
                               WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                               CW_USEDEFAULT, ww, hh, NULL, NULL,
                               GetModuleHandleW(NULL), (LPVOID)w);
    }
    free(wtitle);
    if (hwnd == NULL) {
        CloseHandle(w->done);
        free(w);
        return NULL;
    }
    w->hwnd = hwnd;

    // Resolve the loader without any link-time dependency.
    loader = wv_find_loader();
    if (loader == NULL) {
        goto no_engine;
    }
    create_env = (wv_create_env_fn)(void *)GetProcAddress(
        loader, "CreateCoreWebView2EnvironmentWithOptions");
    if (create_env == NULL) {
        goto no_engine;
    }

    // User-data folder: explicit override, fresh unique dir for
    // private windows, per-process default otherwise.
    tn = GetTempPathW(MAX_PATH, tmp);
    if (wv_g_data_dir_set) {
        wcsncpy(data_dir, wv_g_data_dir,
                (sizeof(data_dir) / sizeof(data_dir[0])) - 1);
        data_dir[(sizeof(data_dir) / sizeof(data_dir[0])) - 1] = L'\0';
        CreateDirectoryW(data_dir, NULL);
        wdata = data_dir;
    } else if (priv) {
        LONG seq = InterlockedIncrement(&wv_g_private_seq);
        if (tn > 0 && tn < MAX_PATH) {
            _snwprintf(data_dir, sizeof(data_dir) / sizeof(data_dir[0]),
                       L"%salya_webview_p%lu_%ld", tmp,
                       (unsigned long)GetCurrentProcessId(), seq);
            data_dir[(sizeof(data_dir) / sizeof(data_dir[0])) - 1] = L'\0';
            CreateDirectoryW(data_dir, NULL);
            wdata = data_dir;
        }
    } else if (tn > 0 && tn < MAX_PATH) {
        _snwprintf(data_dir, sizeof(data_dir) / sizeof(data_dir[0]),
                   L"%salya_webview_%lu", tmp,
                   (unsigned long)GetCurrentProcessId());
        data_dir[(sizeof(data_dir) / sizeof(data_dir[0])) - 1] = L'\0';
        CreateDirectoryW(data_dir, NULL);
        wdata = data_dir;
    }

    eh = wv_make_cb(w, wv_slots_env);
    if (eh == NULL) {
        goto no_engine;
    }
    if (FAILED(create_env(NULL, wdata, NULL, (void *)eh))) {
        wv_cb_rel((void *)eh);
        goto no_engine;
    }
    wv_cb_rel((void *)eh);

    // Wait (pumping) until the controller handshake completes.
    for (;;) {
        wait = MsgWaitForMultipleObjects(1, &w->done, FALSE, 50,
                                         QS_ALLINPUT);
        wv_pump(0);
        if (wait == WAIT_OBJECT_0) {
            break;
        }
        waited += 50;
        if (waited > WV_CREATE_TIMEOUT_MS) {
            goto no_engine;
        }
    }

    if (w->ctl == NULL) {
        goto no_engine;
    }
    if (FAILED(wv_ctl_get_core(w->ctl, &w->core)) || w->core == NULL) {
        goto no_engine;
    }
    // Subscribe: navigation-completed, messages, title.
    {
        wv_cb_t *h1 = wv_make_cb(w, wv_slots_done);
        wv_cb_t *h2 = wv_make_cb(w, wv_slots_msg);
        wv_cb_t *h3 = wv_make_cb(w, wv_slots_title);
        long long tok = 0;
        if (h1 != NULL) {
            if (SUCCEEDED(wv_core_add_completed(w->core, (void *)h1,
                                               (void *)&tok))) {
                wv_cb_rel((void *)h1);
            } else {
                free(h1);
            }
        }
        if (h2 != NULL) {
            if (SUCCEEDED(wv_core_add_message(w->core, (void *)h2,
                                             (void *)&tok))) {
                wv_cb_rel((void *)h2);
            } else {
                free(h2);
            }
        }
        if (h3 != NULL) {
            if (SUCCEEDED(wv_core_add_title(w->core, (void *)h3,
                                            (void *)&tok))) {
                wv_cb_rel((void *)h3);
            } else {
                free(h3);
            }
        }
    }
    w->ready = 1;
    wv_update_bounds(w);
    ShowWindow(hwnd, SW_HIDE);
    return w;

no_engine:
    // Usable handle without an engine: window + event queue still work,
    // browsing calls fail gracefully. Keeps CI green without a runtime.
    w->ready = 0;
    wv_release_com(w);
    ShowWindow(hwnd, SW_HIDE);
    return w;
}

void alya_webview_destroy(alya_webview_t *w) {
    if (w == NULL) {
        return;
    }
    if (w->ctl != NULL) {
        wv_ctl_close(w->ctl);
    }
    wv_release_com(w);
    if (w->hwnd != NULL) {
        DestroyWindow(w->hwnd);
        w->hwnd = NULL;
    }
    if (w->done != NULL) {
        CloseHandle(w->done);
    }
    free(w);
}

void alya_webview_show(alya_webview_t *w) {
    if (w == NULL || w->hwnd == NULL) {
        return;
    }
    ShowWindow(w->hwnd, SW_SHOW);
    if (w->ctl != NULL) {
        wv_ctl_set_visible(w->ctl, 1);
    }
}

void alya_webview_hide(alya_webview_t *w) {
    if (w == NULL || w->hwnd == NULL) {
        return;
    }
    ShowWindow(w->hwnd, SW_HIDE);
    if (w->ctl != NULL) {
        wv_ctl_set_visible(w->ctl, 0);
    }
}

int alya_webview_is_open(alya_webview_t *w) {
    if (w == NULL) {
        return 0;
    }
    return w->open && IsWindow(w->hwnd);
}

int alya_webview_is_ready(alya_webview_t *w) {
    if (w == NULL) {
        return 0;
    }
    return w->ready && w->core != NULL;
}

void alya_webview_request_close(alya_webview_t *w) {
    if (w == NULL) {
        return;
    }
    w->open = 0;
    wv_push(w, ALYA_WEBVIEW_EVENT_CLOSE);
    if (w->hwnd != NULL) {
        PostMessageW(w->hwnd, WM_CLOSE, 0, 0);
    }
}

void alya_webview_set_title(alya_webview_t *w, const char *title) {
    wchar_t *t;
    if (w == NULL || w->hwnd == NULL) {
        return;
    }
    if (title == NULL) {
        title = "";
    }
    wv_copy(w->title, sizeof(w->title), title);
    t = wv_utf8_to_wide(title);
    if (t != NULL) {
        SetWindowTextW(w->hwnd, t);
        free(t);
    }
}

void alya_webview_set_size(alya_webview_t *w, int width, int height) {
    RECT rc;
    if (w == NULL || w->hwnd == NULL) {
        return;
    }
    if (width <= 0 || height <= 0) {
        return;
    }
    // Requested size denotes the client area (mirrors gui's win32
    // backend): the WM_SIZE handler reports the matching client rect.
    rc.left = 0;
    rc.top = 0;
    rc.right = width;
    rc.bottom = height;
    if (AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE)) {
        SetWindowPos(w->hwnd, NULL, 0, 0, rc.right - rc.left,
                     rc.bottom - rc.top, SWP_NOMOVE | SWP_NOZORDER);
    } else {
        SetWindowPos(w->hwnd, NULL, 0, 0, width, height,
                     SWP_NOMOVE | SWP_NOZORDER);
    }
    wv_update_bounds(w);
}

int alya_webview_navigate(alya_webview_t *w, const char *url) {
    wchar_t *u;
    HRESULT hr;
    if (w == NULL || w->core == NULL) {
        return 0;
    }
    if (url == NULL || url[0] == '\0') {
        return 0;
    }
    wv_copy(w->url, sizeof(w->url), url);
    wv_push(w, ALYA_WEBVIEW_EVENT_NAV_START);
    u = wv_utf8_to_wide(url);
    if (u == NULL) {
        return 0;
    }
    hr = wv_core_navigate(w->core, u);
    free(u);
    return SUCCEEDED(hr) ? 1 : 0;
}

int alya_webview_load_html(alya_webview_t *w, const char *html) {
    wchar_t *h;
    HRESULT hr;
    if (w == NULL || w->core == NULL) {
        return 0;
    }
    if (html == NULL) {
        html = "";
    }
    wv_copy(w->url, sizeof(w->url), "about:blank");
    wv_push(w, ALYA_WEBVIEW_EVENT_NAV_START);
    h = wv_utf8_to_wide(html);
    if (h == NULL) {
        return 0;
    }
    hr = wv_core_navigate_to_string(w->core, h);
    free(h);
    return SUCCEEDED(hr) ? 1 : 0;
}

int alya_webview_reload(alya_webview_t *w) {
    if (w == NULL || w->core == NULL) {
        return 0;
    }
    return SUCCEEDED(wv_core_reload(w->core)) ? 1 : 0;
}

int alya_webview_go_back(alya_webview_t *w) {
    if (w == NULL || w->core == NULL) {
        return 0;
    }
    return SUCCEEDED(wv_core_go_back(w->core)) ? 1 : 0;
}

int alya_webview_go_forward(alya_webview_t *w) {
    if (w == NULL || w->core == NULL) {
        return 0;
    }
    return SUCCEEDED(wv_core_go_fwd(w->core)) ? 1 : 0;
}

int alya_webview_can_back(alya_webview_t *w) {
    int v = 0;
    if (w == NULL || w->core == NULL) {
        return 0;
    }
    if (FAILED(wv_core_can_back(w->core, &v))) {
        return 0;
    }
    return v ? 1 : 0;
}

int alya_webview_can_forward(alya_webview_t *w) {
    int v = 0;
    if (w == NULL || w->core == NULL) {
        return 0;
    }
    if (FAILED(wv_core_can_fwd(w->core, &v))) {
        return 0;
    }
    return v ? 1 : 0;
}

int alya_webview_eval(alya_webview_t *w, const char *js) {
    wchar_t *j;
    wv_cb_t *h;
    HRESULT hr;
    if (w == NULL || w->core == NULL) {
        return 0;
    }
    if (js == NULL || js[0] == '\0') {
        return 0;
    }
    j = wv_utf8_to_wide(js);
    if (j == NULL) {
        return 0;
    }
    h = wv_make_cb(w, wv_slots_script);
    if (h == NULL) {
        free(j);
        return 0;
    }
    w->eval_state = ALYA_WEBVIEW_EVAL_PENDING;
    w->eval_result[0] = '\0';
    hr = wv_core_execute_script(w->core, j, (void *)h);
    free(j);
    wv_cb_rel((void *)h); // WebView2 holds its own reference now
    return SUCCEEDED(hr) ? 1 : 0;
}

int alya_webview_eval_state(alya_webview_t *w) {
    if (w == NULL) {
        return ALYA_WEBVIEW_EVAL_ERROR;
    }
    return w->eval_state;
}

int alya_webview_post_message(alya_webview_t *w, const char *json) {
    wchar_t *j;
    HRESULT hr;
    if (w == NULL || w->core == NULL) {
        return 0;
    }
    if (json == NULL) {
        json = "";
    }
    j = wv_utf8_to_wide(json);
    if (j == NULL) {
        return 0;
    }
    hr = wv_core_post_message(w->core, j);
    free(j);
    return SUCCEEDED(hr) ? 1 : 0;
}

int alya_webview_set_devtools(alya_webview_t *w, int enabled) {
    void *s = NULL;
    HRESULT hr;
    if (w == NULL || w->core == NULL) {
        return 0;
    }
    if (FAILED(wv_core_get_settings(w->core, &s)) || s == NULL) {
        return 0;
    }
    hr = wv_settings_put_devtools(s, enabled ? 1 : 0);
    wv_iface_release(s);
    return SUCCEEDED(hr) ? 1 : 0;
}

int alya_webview_set_js(alya_webview_t *w, int enabled) {
    // Not wired: the script-toggle slot is unverified on current
    // runtimes (see slot-map note above). JavaScript ships enabled in
    // WebView2/WebKitGTK/WKWebView; report unsupported instead of
    // risking a fault. Re-enable after SDK-header verification.
    (void)w;
    (void)enabled;
    return 0;
}

int alya_webview_set_user_agent(alya_webview_t *w, const char *ua) {
    // User-agent override needs ICoreWebView2Settings2; v1 keeps the
    // stable base Settings surface only and reports unsupported here.
    (void)w;
    (void)ua;
    return 0;
}

/* Synthetic input: posted to the WebView2 content child window.
 * Pure Win32, no COM involved. Keys need keyboard focus on the child;
 * mouse events work regardless of focus. */

static BOOL CALLBACK wv_enum_child(HWND h, LPARAM lp) {
    wchar_t cls[64];
    if (GetClassNameW(h, cls, 64) > 0 &&
        wcscmp(cls, L"Chrome_WidgetWin_1") == 0) {
        *(HWND *)lp = h;
        return FALSE;
    }
    return TRUE;
}

static HWND wv_content_child(alya_webview_t *w) {
    if (w == NULL || w->hwnd == NULL) {
        return NULL;
    }
    if (w->child != NULL && IsWindow(w->child)) {
        return w->child;
    }
    w->child = NULL;
    EnumChildWindows(w->hwnd, wv_enum_child, (LPARAM)&w->child);
    return w->child;
}

int alya_webview_mouse_move(alya_webview_t *w, int x, int y) {
    HWND c;
    if (w == NULL) {
        return 0;
    }
    w->mouse_x = x;
    w->mouse_y = y;
    c = wv_content_child(w);
    if (c == NULL) {
        return 0;
    }
    return PostMessageW(c, WM_MOUSEMOVE, 0, MAKELPARAM(x, y)) ? 1 : 0;
}

static int wv_mouse_btn(alya_webview_t *w, int button, int down) {
    HWND c;
    UINT msg = 0;
    WPARAM flags = 0;
    if (w == NULL) {
        return 0;
    }
    if (button == 0) {
        msg = down ? WM_LBUTTONDOWN : WM_LBUTTONUP;
        flags = MK_LBUTTON;
    } else if (button == 1) {
        msg = down ? WM_RBUTTONDOWN : WM_RBUTTONUP;
        flags = MK_RBUTTON;
    } else if (button == 2) {
        msg = down ? WM_MBUTTONDOWN : WM_MBUTTONUP;
        flags = MK_MBUTTON;
    } else {
        return 0;
    }
    c = wv_content_child(w);
    if (c == NULL) {
        return 0;
    }
    if (!down) {
        flags = 0;
    }
    return PostMessageW(c, msg, flags,
                        MAKELPARAM(w->mouse_x, w->mouse_y))
               ? 1
               : 0;
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
    HWND c;
    int posted = 0;
    if (w == NULL) {
        return 0;
    }
    c = wv_content_child(w);
    if (c == NULL) {
        return 0;
    }
    if (dy != 0 && PostMessageW(c, WM_MOUSEWHEEL,
                                MAKEWPARAM(0, (short)(dy * 120)),
                                MAKELPARAM(w->mouse_x, w->mouse_y))) {
        posted = 1;
    }
    if (dx != 0 && PostMessageW(c, WM_MOUSEHWHEEL,
                                MAKEWPARAM(0, (short)(dx * 120)),
                                MAKELPARAM(w->mouse_x, w->mouse_y))) {
        posted = 1;
    }
    return posted;
}

static int wv_is_extended_vk(int code) {
    switch (code) {
    case VK_LEFT:
    case VK_RIGHT:
    case VK_UP:
    case VK_DOWN:
    case VK_HOME:
    case VK_END:
    case VK_INSERT:
    case VK_DELETE:
    case VK_PRIOR:
    case VK_NEXT:
        return 1;
    default:
        return 0;
    }
}

static int wv_post_key(alya_webview_t *w, int code, int down) {
    HWND c;
    UINT sc;
    LPARAM lp;
    if (w == NULL || code <= 0 || code > 255) {
        return 0;
    }
    c = wv_content_child(w);
    if (c == NULL) {
        return 0;
    }
    sc = MapVirtualKeyW((UINT)code, MAPVK_VK_TO_VSC);
    lp = 1 | ((LPARAM)sc << 16);
    if (wv_is_extended_vk(code)) {
        lp |= (1 << 24);
    }
    if (!down) {
        lp |= ((LPARAM)1 << 30) | ((LPARAM)1 << 31);
    }
    return PostMessageW(c, down ? WM_KEYDOWN : WM_KEYUP, (WPARAM)code, lp)
               ? 1
               : 0;
}

int alya_webview_key_down(alya_webview_t *w, int code) {
    return wv_post_key(w, code, 1);
}

int alya_webview_key_up(alya_webview_t *w, int code) {
    return wv_post_key(w, code, 0);
}

int alya_webview_key_tap(alya_webview_t *w, int code) {
    int d;
    int u;
    if (w == NULL) {
        return 0;
    }
    d = wv_post_key(w, code, 1);
    u = wv_post_key(w, code, 0);
    return (d && u) ? 1 : 0;
}

int alya_webview_key_text(alya_webview_t *w, const char *text) {
    HWND c;
    wchar_t *u;
    size_t i;
    size_t n;
    int posted = 0;
    if (w == NULL || text == NULL || text[0] == '\0') {
        return 0;
    }
    c = wv_content_child(w);
    if (c == NULL) {
        return 0;
    }
    u = wv_utf8_to_wide(text);
    if (u == NULL) {
        return 0;
    }
    n = wcslen(u);
    for (i = 0; i < n; i++) {
        if (PostMessageW(c, WM_CHAR, (WPARAM)u[i], 1)) {
            posted = 1;
        }
    }
    free(u);
    return posted;
}

int alya_webview_key_code(const char *name) {
    static const struct {
        const char *name;
        int code;
    } map[] = {{"Enter", VK_RETURN},     {"Escape", VK_ESCAPE},
               {"Tab", VK_TAB},          {"Backspace", VK_BACK},
               {"Delete", VK_DELETE},    {"Left", VK_LEFT},
               {"Up", VK_UP},            {"Right", VK_RIGHT},
               {"Down", VK_DOWN},        {"Home", VK_HOME},
               {"End", VK_END},          {"PageUp", VK_PRIOR},
               {"PageDown", VK_NEXT},    {NULL, -1}};
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
    // ZoomFactor vtable slots are unverified on current runtimes
    // (see slot-map note above): report unsupported, never fault.
    (void)w;
    (void)factor;
    return 0;
}

double alya_webview_get_zoom(alya_webview_t *w) {
    (void)w;
    return 0.0;
}

int alya_webview_poll(alya_webview_t *w) {
    int kind;
    MSG msg;
    if (w == NULL) {
        return ALYA_WEBVIEW_EVENT_NONE;
    }
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
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
