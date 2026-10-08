/* Alya webview native contract (v0.1.0).
 *
 * One header for all three OS backends. Each backend compiles exactly one
 * of c/webview_win.c (WebView2), c/webview_mac.c (WKWebView),
 * c/webview_linux.c (WebKitGTK) plus this shared translation unit.
 *
 * Design rules (do not break without a major version bump):
 * - Handles are opaque; Alya only ever passes them back (never inspects).
 * - All string getters return a non-NULL, NUL-terminated buffer owned by
 *   the handle. Copy it on the Alya side when it must outlive the call.
 * - All functions are null-safe: NULL handles / NULL strings are ignored
 *   (getters fall back to "" / 0). This keeps headless CI green.
 * - No C-to-Alya callbacks. Async browser work (JS eval, navigation,
 *   messages) lands in a per-window ring queue drained via poll().
 */

#ifndef ALYA_WEBVIEW_H
#define ALYA_WEBVIEW_H

/* Event kinds returned by alya_webview_poll(). */
#define ALYA_WEBVIEW_EVENT_NONE 0
#define ALYA_WEBVIEW_EVENT_CLOSE 1
#define ALYA_WEBVIEW_EVENT_RESIZE 2
#define ALYA_WEBVIEW_EVENT_NAV_START 3
#define ALYA_WEBVIEW_EVENT_NAV_DONE 4
#define ALYA_WEBVIEW_EVENT_TITLE 5
#define ALYA_WEBVIEW_EVENT_MESSAGE 6

/* JS eval states returned by alya_webview_eval_state(). */
#define ALYA_WEBVIEW_EVAL_PENDING 0
#define ALYA_WEBVIEW_EVAL_READY 1
#define ALYA_WEBVIEW_EVAL_ERROR 2

/* Payload capacities (bytes, including NUL). */
#define ALYA_WEBVIEW_TEXT_CAP 1024
#define ALYA_WEBVIEW_URL_CAP 2048

#ifdef __cplusplus
extern "C" {
#endif

/* Bundled-C smoke test: proves the native objects linked correctly. */
int alya_webview_add(int a, int b);

/* Compiled backend id: 1 = Windows/WebView2, 2 = macOS/WKWebView,
 * 3 = Linux/WebKitGTK. */
int alya_webview_backend_id(void);

/* Backend name for the compiled target: "windows", "macos", "linux". */
const char *alya_webview_backend_name(void);

/* Lifetime. Returns NULL when no display, runtime, or engine is
 * available (headless CI, missing WebView2 Runtime, ...). */
typedef struct alya_webview alya_webview_t;
alya_webview_t *alya_webview_create(const char *title, int width, int height);

/* Private (incognito) window: no persistent profile is kept.
 * Windows: fresh unique user-data dir. macOS: non-persistent store.
 * Linux: ephemeral web context. NULL handling matches create(). */
alya_webview_t *alya_webview_create_private(const char *title, int width,
                                            int height);

/* Launch configuration. Must be called before open()/open_private();
 * process-wide, last call wins. data_dir overrides the profile folder
 * (Windows/Linux; ignored on macOS). extra_args forwards Chromium
 * switches (Windows WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS only). */
void alya_webview_set_data_dir(const char *path);
void alya_webview_set_extra_args(const char *args);
void alya_webview_destroy(alya_webview_t *w);
void alya_webview_show(alya_webview_t *w);
void alya_webview_hide(alya_webview_t *w);
int alya_webview_is_open(alya_webview_t *w);
int alya_webview_is_ready(alya_webview_t *w);
void alya_webview_request_close(alya_webview_t *w);
void alya_webview_set_title(alya_webview_t *w, const char *title);
void alya_webview_set_size(alya_webview_t *w, int width, int height);

/* Browsing. Return 1 when accepted, 0 when ignored (NULL / no engine). */
int alya_webview_navigate(alya_webview_t *w, const char *url);
int alya_webview_load_html(alya_webview_t *w, const char *html);
int alya_webview_reload(alya_webview_t *w);
int alya_webview_go_back(alya_webview_t *w);
int alya_webview_go_forward(alya_webview_t *w);
int alya_webview_can_back(alya_webview_t *w);
int alya_webview_can_forward(alya_webview_t *w);

/* Scripting. eval() fires asynchronously; the JSON-encoded result is
 * stashed and reported via eval_state()/eval_result(). post_message()
 * delivers a string to JS (WebView2 postMessage / WKScriptMessage /
 * webkit_user_content_manager equivalent bootstrap page). */
int alya_webview_eval(alya_webview_t *w, const char *js);
int alya_webview_eval_state(alya_webview_t *w);
int alya_webview_post_message(alya_webview_t *w, const char *json);

/* Settings. Return 1 when applied, 0 when ignored. */
int alya_webview_set_devtools(alya_webview_t *w, int enabled);
int alya_webview_set_js(alya_webview_t *w, int enabled);
int alya_webview_set_user_agent(alya_webview_t *w, const char *ua);

/* Cooperative pump. Runs pending OS work without blocking and returns
 * the oldest queued event kind (ALYA_WEBVIEW_EVENT_*). Payload of the
 * returned event is visible through the event_* getters until the next
 * poll() call. */
/* Best-effort launch of a URL in the user's default browser.
 * Opt-in fallback for machines without an embeddable engine: never
 * blocks, never throws. Returns 1 when the launch was attempted,
 * 0 when it was not (NULL/empty URL, headless helpers missing). */
int alya_webview_open_external(const char *url);

/* Embeddable engine version ("1.0.4258.31", "4.1.3", ...), "" when the
 * engine cannot be probed. Never NULL. */
const char *alya_webview_engine_version(void);

/* Zoom factor (1.0 = 100%). set returns 1 when applied; get returns
 * the current factor, 0.0 when unknown. Backends without a safe zoom
 * path report 0/0.0 instead of risking a fault (see backend notes). */
int alya_webview_set_zoom(alya_webview_t *w, double factor);
double alya_webview_get_zoom(alya_webview_t *w);

/* Synthetic input, in window client pixels (origin top-left).
 * button: 0 = left, 1 = right, 2 = middle. code: platform key code
 * (see alya_webview_key_code); key_text commits printable text.
 * Delivery targets the embedded page only and never blocks.
 * Each function returns 1 when the event was posted, 0 otherwise. */
int alya_webview_mouse_move(alya_webview_t *w, int x, int y);
int alya_webview_mouse_down(alya_webview_t *w, int button);
int alya_webview_mouse_up(alya_webview_t *w, int button);
int alya_webview_mouse_click(alya_webview_t *w, int button);
int alya_webview_mouse_wheel(alya_webview_t *w, int dx, int dy);
int alya_webview_key_down(alya_webview_t *w, int code);
int alya_webview_key_up(alya_webview_t *w, int code);
int alya_webview_key_tap(alya_webview_t *w, int code);
int alya_webview_key_text(alya_webview_t *w, const char *text);

/* Maps common key names ("Enter", "Escape", "Tab", "Backspace",
 * "Delete", "Left", "Up", "Right", "Down", "Home", "End",
 * "PageUp", "PageDown") to the platform code. Returns -1 when unknown. */
int alya_webview_key_code(const char *name);

/* Content policy toggles. Each returns 1 when applied, 0 when the
 * backend cannot honor it (unsupported generation, missing engine).
 * - background: page base color, components 0..255 (alpha honored
 *   where compositing allows; ignored on fully opaque windows).
 * - context_menu: 1 shows the native menu, 0 suppresses it.
 * - shortcut_block: 1 swallows app-level shortcuts in the page
 *   (Ctrl+P, PrintScreen, F12, Ctrl+Shift+I/J/C, Ctrl+U).
 * - images: 1 loads images, 0 blocks them.
 * - webgl: 1 enables WebGL, 0 disables it.
 * - charset: default text encoding name ("UTF-8"); "" leaves default. */
int alya_webview_set_background(alya_webview_t *w, int r, int g, int b,
                                int a);
int alya_webview_set_context_menu(alya_webview_t *w, int enabled);
int alya_webview_set_shortcut_block(alya_webview_t *w, int enabled);
int alya_webview_set_images(alya_webview_t *w, int enabled);
int alya_webview_set_webgl(alya_webview_t *w, int enabled);
int alya_webview_set_charset(alya_webview_t *w, const char *cs);

int alya_webview_poll(alya_webview_t *w);
int alya_webview_event_width(alya_webview_t *w);
int alya_webview_event_height(alya_webview_t *w);
const char *alya_webview_event_url(alya_webview_t *w);
const char *alya_webview_event_title(alya_webview_t *w);
const char *alya_webview_event_text(alya_webview_t *w);
const char *alya_webview_eval_result(alya_webview_t *w);

#ifdef __cplusplus
}
#endif

#endif
