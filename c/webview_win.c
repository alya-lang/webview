#include "webview.h"

// Windows backend stub (WebView2 loader goes here).
// Real implementation will create ICoreWebView2Controller,
// Navigate, ExecuteScript and post WebMessages.
int alya_webview_backend_id(void) {
    return 1;
}
