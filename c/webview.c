/* Shared translation unit for the Alya webview package.
 *
 * Compiled on every target alongside exactly one per-OS backend.
 * Holds engine-independent helpers only; all OS work lives in the
 * c/webview_{win,mac,linux}.c backends.
 */

#include "webview.h"

int alya_webview_add(int a, int b) {
    return a + b;
}
