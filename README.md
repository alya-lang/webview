# webview

[![CI](https://github.com/alya-lang/webview/actions/workflows/ci.yml/badge.svg)](https://github.com/alya-lang/webview/actions/workflows/ci.yml)
[![License](https://img.shields.io/github/license/alya-lang/webview?color=blue&label=License)](LICENSE)
[![Alya](https://img.shields.io/badge/dynamic/toml?url=https%3A%2F%2Fraw.githubusercontent.com%2Falya-lang%2Fwebview%2Fmain%2Falya.toml&query=%24.package.alya-version&label=Alya&color=orange&prefix=%3E%3D)](https://github.com/alya-lang/alya)
[![Package Version](https://img.shields.io/badge/dynamic/toml?url=https%3A%2F%2Fraw.githubusercontent.com%2Falya-lang%2Fwebview%2Fmain%2Falya.toml&query=%24.package.version&label=Version&color=brightgreen)](alya.toml)

Cross-platform system webview bindings for Alya: WebView2 on Windows, WKWebView on macOS, WebKitGTK on Linux

---

## 🌟 Features

- 🖥️ **System Engines, Zero Bundling**: WebView2 (Windows), WKWebView (macOS), WebKitGTK (Linux) — no Chromium download, no C++ toolchain, no SDK headers required
- 🧩 **One C Contract, Three Backends**: `c/webview.h` is the single ABI; `c/webview_win.c`, `c/webview_mac.c`, `c/webview_linux.c` implement it (pure C, runtime loader/engine probing via `LoadLibrary` / `dlopen`)
- 🔌 **FFI Without Callbacks**: async browser work (navigation, JS eval, page messages) arrives through `drain()` as `WebviewEvent` records — no C-to-Alya closures needed
- 🛡️ **Null-Safe & Headless-Clean**: every function tolerates null handles; `open()` returns null without a display or engine, so CI stays green on all 6 targets
- ⚙️ **Engine Settings**: devtools toggle, JavaScript toggle, custom user-agent, `window.postMessage` delivery both directions
- 📦 **No Link-Time Surprises**: Windows links only system DLLs (`ole32`, `user32`, `gdi32`, `shell32`); macOS links system frameworks; Linux links only `libdl`

> [!NOTE]
> **Platform requirements:** Windows needs the evergreen WebView2 Runtime (preinstalled with Edge); macOS ships WKWebView; Linux needs `libwebkit2gtk-4.1` (or `4.0`) plus a display. Without them, browsing calls report "not ready" instead of failing.

---

## 📁 Project Architecture

```
webview/
├── .alyalint               # Linter configuration (rules, exclusions, severity overrides)
├── .editorconfig           # Uniform formatting rules across IDEs and editors
├── .gitignore              # Ecosystem standard ignore filters
├── .vscode/                # VS Code workspace settings, DAP launch configurations & tasks
├── alya.toml               # Package manifest with per-OS [build] sources and link flags
├── c/                      # Native backends (zero-dependency FFI)
│   ├── webview.h           # Single shared ABI contract (all backends implement it)
│   ├── webview.c           # Engine-independent helpers (link smoke test)
│   ├── webview_win.c       # Windows: WebView2 via manual COM vtables + runtime loader
│   ├── webview_mac.c       # macOS: WKWebView via ObjC runtime C API (no ObjC syntax)
│   └── webview_linux.c     # Linux: WebKitGTK 4.1/4.0 via dlopen (no headers needed)
├── src/
│   ├── lib.alya            # Public API facade (open, navigate, eval, drain, settings)
│   ├── types.alya          # WebviewEventKind, WebviewEvalState, WebviewEvent, WebviewSettings
│   ├── ffi.alya            # Bundled-C smoke-test declarations
│   ├── core/
│   │   └── events.alya     # FIFO WebviewEventQueue (push/peek/poll/clear)
│   └── native/
│       └── browser.alya    # Null-safe extern "C" bindings + drain/eval_wait helpers
├── examples/
│   └── demo.alya           # Headless-safe showcase (skips live window without display)
├── tests/
│   ├── test_basic.alya     # Pure logic: types, queue, settings, null-safety, FFI smoke
│   └── test_native.alya    # Live window: resize/nav/eval/close (skips headless)
└── benches/
    └── bench_basic.alya    # Micro-benchmarks (pure Alya, no windows)
```

> [!NOTE]
> **Visibility & Modularity:** Symbols annotated with `pub` (`pub function`, `pub struct`, `pub enum`) are exported to external consumers and re-exporting modules. Symbols without `pub` remain strictly internal to their declaring module, preventing symbol collisions and implementation leakage.

---

## 📦 Installation

Add `webview` to the `[dependencies]` section in your `alya.toml`:

```toml
[dependencies]
webview = { git = "https://github.com/alya-lang/webview", branch = "main" }
```

Or install it directly using the Alya package CLI:

```bash
alya add webview --git https://github.com/alya-lang/webview --branch main
alya install
```

---

## 🚀 Quick Start

```alya
import "webview" as pkg

function main()
    say "Backend: " + pkg::backend()

    # Headless-safe: null when no display or engine is present.
    let w = pkg::open("Hello webview", 1024, 768)
    if w is null
        say "No display or engine — live view skipped."
        return
    end

    pkg::apply_settings(w, pkg::settings())
    pkg::load_html(w, "<html><body><h1>Hello from Alya</h1></body></html>")

    # Wait for the page, then run JavaScript (JSON-encoded result).
    let t0 = clock_ms()
    while clock_ms() - t0 < 20000
        if pkg::poll(w) == pkg::WebviewEventKind.NavDone
            break
        end
        sleep(10)
    end
    say "JS says: " + pkg::eval_wait(w, "40+2", 800)

    # Drain remaining events as structured records.
    let q = pkg::event_queue()
    pkg::drain(w, q)
    while pkg::event_queue_len(q) > 0
        let ev = pkg::event_queue_poll(q)
        say ev.summary()
    end

    pkg::request_close(w)
    pkg::destroy(w)
end

main()
```

---

## 📖 API Reference

| Symbol | Visibility | Description |
|---|---|---|
| `backend()` | `pub function` | Returns `"windows"`, `"macos"`, or `"linux"` for the compiled target. |
| `engine_version()` | `pub function` | Embeddable engine version (`""` when not probeable). |
| `backend_id()` | `pub function` | Returns `1` (Windows), `2` (macOS), or `3` (Linux). |
| `open(title, width, height)` | `pub function` | Opens a browser window; null without display or engine. |
| `open_private(title, width, height)` | `pub function` | Opens an incognito window (no persistent profile). |
| `set_data_dir(path)` | `pub function` | Profile folder for subsequently opened windows (Win/Linux). |
| `data_dir()` | `pub function` | Configured profile folder (`""` when default). |
| `set_extra_args(args)` | `pub function` | Extra Chromium switches, e.g. remote debugging (Win). |
| `extra_args()` | `pub function` | Configured extra switches (`""` when none). |
| `profile_path(win)` | `pub function` | Actual folder backing an open window (`""` when none). |
| `set_extra_args(args)` | `pub function` | Extra Chromium switches for subsequently opened windows (Win). |
| `set_zoom(win, factor)` | `pub function` | Page zoom (`1.0` = 100%, `1` when applied). |
| `get_zoom(win)` | `pub function` | Current zoom factor (`0.0` when unknown). |
| `mouse_move(win, x, y)` | `pub function` | Synthetic cursor move over client pixels. |
| `mouse_down(win, button)` / `mouse_up(win, button)` | `pub function` | Synthetic button press/release (`0` left, `1` right, `2` middle). |
| `mouse_click(win, button)` | `pub function` | Synthetic click (press + release). |
| `mouse_wheel(win, dx, dy)` | `pub function` | Synthetic scroll detents. |
| `key_down(win, code)` / `key_up(win, code)` | `pub function` | Synthetic key press/release (platform code; needs focus). |
| `key_tap(win, code)` | `pub function` | Synthetic key tap (press + release). |
| `key_text(win, text)` | `pub function` | Commits printable text to the page. |
| `key_code(name)` | `pub function` | Maps `"Enter"`, `"Escape"`, `"Tab"`, … to the platform code (`-1` unknown). |
| `set_background(win, r, g, b, a)` | `pub function` | Page base color (`1` when applied). |
| `set_context_menu(win, enabled)` | `pub function` | Native context menu on/off (`1` when applied). |
| `set_shortcut_block(win, enabled)` | `pub function` | Blocks Ctrl+P / PrintScreen / F12 / … in the page (`1` when applied). |
| `set_images(win, enabled)` | `pub function` | Image loading on/off (`1` when applied). |
| `set_webgl(win, enabled)` | `pub function` | WebGL on/off (`1` when applied). |
| `set_charset(win, cs)` | `pub function` | Default text encoding (`1` when applied). |
| `open_external(url)` | `pub function` | Opens a URL in the default browser; `1` when attempted (opt-in fallback). |
| `destroy(win)` | `pub function` | Destroys a handle (null-safe, always `1`). |
| `show(win)` / `hide(win)` | `pub function` | Shows or hides a window (null-safe). |
| `is_open(win)` | `pub function` | Returns `1` while the handle is open. |
| `is_ready(win)` | `pub function` | Returns `1` when the engine is attached (browsing works). |
| `request_close(win)` | `pub function` | Requests graceful close, queues a `Close` event (null-safe). |
| `set_title(win, title)` | `pub function` | Sets the window title (null-safe). |
| `resize(win, width, height)` | `pub function` | Resizes the client area, queues a `Resize` event (null-safe). |
| `navigate(win, url)` | `pub function` | Navigates to a URL; `1` when accepted. |
| `load_html(win, html)` | `pub function` | Loads an HTML document string; `1` when accepted. |
| `reload(win)` | `pub function` | Reloads the current page; `1` when accepted. |
| `back(win)` / `forward(win)` | `pub function` | History traversal; `1` when accepted. |
| `can_back(win)` / `can_forward(win)` | `pub function` | Returns `1` when history traversal is possible. |
| `eval(win, js)` | `pub function` | Submits JS asynchronously; `1` when submitted. |
| `eval_state(win)` | `pub function` | `0` pending, `1` ready, `2` error (`2` for null). |
| `eval_result(win)` | `pub function` | Last JSON-encoded JS result (`""` when none). |
| `eval_wait(win, js, budget)` | `pub function` | Submits JS and waits up to `budget` pumps; `""` on timeout. |
| `post_message(win, json)` | `pub function` | Delivers a string via `window.postMessage`; `1` when delivered. |
| `set_devtools(win, enabled)` | `pub function` | Toggles developer tools; `1` when applied. |
| `set_js(win, enabled)` | `pub function` | Toggles JavaScript; `0` where the backend cannot toggle it. |
| `set_user_agent(win, ua)` | `pub function` | Overrides the user-agent; `1` when applied. |
| `apply_settings(win, s)` | `pub function` | Applies a `WebviewSettings` record; returns applied count. |
| `settings()` | `pub function` | Default settings (devtools off, JS on, default UA). |
| `settings_with(devtools, js, user_agent)` | `pub function` | Explicit settings constructor. |
| `poll(win)` | `pub function` | Pumps once; returns the event kind (`0` = none). |
| `drain(win, q)` | `pub function` | Drains pending events into `q` as `WebviewEvent` records. |
| `event_queue()` | `pub function` | Creates an empty `WebviewEventQueue`. |
| `last_url(win)` / `last_title(win)` / `last_text(win)` | `pub function` | Last URL, document title, message payload (`""` when unknown). |
| `c_add(a, b)` | `pub function` | Bundled-C link smoke test (`10 + 32 == 42`). |
| `WebviewEventKind` | `pub enum` | `None = 0`, `Close = 1`, `Resize = 2`, `NavStart = 3`, `NavDone = 4`, `Title = 5`, `Message = 6`. |
| `WebviewEvalState` | `pub enum` | `Pending = 0`, `Ready = 1`, `Error = 2`. |
| `WebviewEvent` | `pub struct` | Drained event (`kind`, `width`, `height`, `url`, `title`, `text`) with `is_close()`, `is_message()`, `summary()`. |
| `WebviewSettings` | `pub struct` | Engine settings (`devtools`, `js`, `user_agent`). |
| `WebviewEventQueue` | `pub struct` | FIFO queue with `event_queue_push/peek/poll/len/is_empty/clear`. |

### 🔭 Roadmap (v1.1)

- Re-verify the remaining WebView2 vtable slots against real SDK headers (script toggle, user-agent via `Settings2`, zoom factor, background color, context menu, dialogs).
- Drive-by verification of the macOS (`WKWebView`) and Linux (`WebKitGTK`) backends on native runners.
- Cookies (`CookieManager`), downloads (`DownloadStarting` + destination), DevTools protocol (`CallDevToolsProtocolMethod`), print-to-PDF, page capture, script dialogs, permission requests, custom schemes.
- Page-side JS bridge helper (unified `window.alya` inbox on top of the platform channels).

> [!TIP]
> **Internal Helpers & Documentation:** Public symbols are documented with `##` Markdown docstrings, enabling automatic API documentation generation via `alya doc`. Raw `extern "C"` declarations in `src/native/browser.alya` stay private behind null-safe wrappers.

---

## 🧪 Running Tests & Benchmarks

Run the automated test suite using `alya test`:

```bash
alya test
```

Generate static API documentation:

```bash
alya doc . -o docs --markdown
```

Run the benchmark suite:

```bash
alya run benches/bench_basic.alya
```

Run the example demo:

```bash
alya run examples/demo.alya
```

Check code formatting:

```bash
alya fmt . --check
```

Run static code linter:

```bash
alya lint . --check
```

---

### 💻 Developer Tooling & VS Code Integration

This package comes preconfigured with recommended workspace settings and tasks for **Visual Studio Code**:
- **LSP & Formatting**: Auto-formatting on save and real-time Language Server diagnostics via `alya-lang.vscode-alya`.
- **DAP Debugging**: Launch configurations in `.vscode/launch.json` ready for interactive step-debugging via `F5`.
- **Predefined Tasks**: Press `Ctrl+Shift+B` or run tasks (`Test`, `Lint`, `Format`, `Build Docs`) directly from the Command Palette.

---

## 🤝 Contributing

Contributions are welcome! Please follow these steps:

1. Fork the repository and clone it locally
2. Install dependencies:
   ```bash
   alya install
   ```
3. Create your feature branch (`git checkout -b feature/my-feature`)
4. Verify tests and formatting before opening a PR:
   ```bash
   alya test
   ```
5. Commit your changes (`git commit -m "feat: add feature"`) and open a Pull Request

---

## 📄 License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.
