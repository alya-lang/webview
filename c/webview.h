#ifndef ALYA_Webview_H
#define ALYA_Webview_H

int alya_webview_add(int a, int b);

// Returns the native backend id for the compiled target:
// 1 = Windows (WebView2), 2 = macOS (WKWebView), 3 = Linux (WebKitGTK).
// Implemented per-OS in c/webview_win.c, c/webview_mac.c, c/webview_linux.c.
int alya_webview_backend_id(void);

#endif