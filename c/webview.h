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
