# Minima — cross-platform porting plan

> Copyright (c) 2026 g-halcyon (https://github.com/g-halcyon). Licensed under [PolyForm Noncommercial License 1.0](LICENSE).

Goal: run Minima on **Windows, macOS, Linux, and Android** from **one C++ codebase**,
without Rust/Tauri and without bundling a second copy of Chromium. This document is
the contract the code is being refactored toward, so that platform work done now is
not thrown away later.

## The core idea: portable C++ core + thin native shells

A browser like Minima splits cleanly into two layers:

```
        ┌─────────────────────────────────────────────┐
        │  CORE  (portable C++17 — one copy, all OSes)  │
        │                                               │
        │  • Tab / Bookmark / History / Settings models │
        │  • JSON read/write, profile storage paths     │
        │  • Search-engine + ad/tracker host logic      │
        │  • Curated extension catalog + resolver       │
        │  • On-device AI orchestration (sidecar mgmt)  │
        │  • ALL of the UI: the chrome strip, start /   │
        │    history / settings / wizard / AI / sidebar │
        │    pages are HTML+CSS+JS strings — 100% port. │
        └───────────────▲───────────────────────────────┘
                        │  platform.h  (abstract interface)
        ┌───────────────┴───────────────────────────────┐
        │  SHELL  (small, per-OS, native language)       │
        │                                                │
        │  win    : Win32 window + WebView2  (C++)   ✅  │
        │  macos  : NSWindow + WKWebView   (Obj-C++)     │
        │  linux  : GTK window + WebKitGTK (C++)         │
        │  android: Activity + android.webkit.WebView    │
        │           (Kotlin/Java) ↔ core over JNI        │
        └────────────────────────────────────────────────┘
```

Everything above `platform.h` is written once. Each shell is a few hundred lines that
(1) opens a native window, (2) hosts one webview per tab + one for the chrome UI, and
(3) forwards navigation/keyboard/lifecycle events into the core. The UI itself —
already authored as self-contained HTML/JS strings in `src/main.cpp` — moves across
platforms **verbatim**. That is the bulk of the product and it is already portable.

## What is portable *today* vs. Windows-only *today*

`src/main.cpp` is currently the Windows shell and the core fused together. Roughly:

| Portable as-is (→ `core/`) | Windows-only (→ `platform/windows/`) |
|---|---|
| Every `k…Html` string + `Build…Html()` builder | `ICoreWebView2*` calls, `ComPtr`, WRL `Callback` |
| `Tab`, `Settings`, `Bookmark`, `History` structs | `HWND`, `WndProc`, the Win32 message loop |
| JSON parse/serialize, `%profile%` layout | `WinHttp*` (→ `IHttpClient`) |
| `kSearchEngines`, ad-host list, `IsAdUrl` | `CreateProcessW` for the AI sidecar (→ `IProcess`) |
| Curated catalog + `FindGithubAssetUrl` | `SHGetKnownFolderPath`, DPI, DWM theming |
| AI phase/state machine | `AddBrowserExtension` (see caveat below) |

The refactor is mechanical: move the left column into `core/`, replace each direct
WebView2/Win32/WinHTTP call with a `platform.h` interface call, and implement that
interface once per OS.

## ⚠️ The one hard platform difference: extensions

Chrome-extension support (`AddBrowserExtension`, the curated catalog) exists **only on
the Chromium engine**, i.e. WebView2 on Windows and `android.webkit.WebView` is *also*
Chromium — but neither macOS `WKWebView` nor Linux `WebKitGTK` can load Chrome
extensions at all. So:

- **Windows**: full extension catalog. ✅ (shipping)
- **Android**: WebView is Chromium, but the system WebView deliberately disables the
  extensions API; treat extensions as **unavailable** unless we later embed a custom
  Chromium (out of scope — it would blow up the app size, against Minima's ethos).
- **macOS / Linux**: extensions **unavailable** on the system webview.

`platform.h` exposes `supportsExtensions()` so the Settings UI hides the catalog where
it can't work. Ad/tracker blocking does **not** depend on this — it's done in the core
by intercepting requests, so it works everywhere.

## Per-platform webview backend

| OS | Window | Webview | Shell language | Request intercept (adblock) |
|----|--------|---------|----------------|------------------------------|
| Windows | Win32 `HWND` | WebView2 | C++ | `WebResourceRequested` |
| macOS | `NSWindow` | `WKWebView` | Objective-C++ (`.mm`) | `WKURLSchemeHandler` / `WKContentRuleList` |
| Linux | GTK `GtkWindow` | `WebKitGTK` (`WebKitWebView`) | C++ + GObject | `WebKitWebResource` signal / `resource-request-starting` |
| Android | `Activity` | `android.webkit.WebView` | Kotlin/Java + JNI | `shouldInterceptRequest` |

Each backend renders the **same** chrome HTML and the same per-tab webviews; only the
host container and the event plumbing differ.

## Build system

`CMakeLists.txt` (added) is the single cross-platform build. It selects the shell from
the target OS:

```bash
# Desktop (Windows / macOS / Linux)
cmake -B build -S .
cmake --build build

# Android: built via Gradle, which invokes CMake through the NDK
cd android && ./gradlew assembleDebug
```

`build.bat` remains as the zero-dependency Windows path (no CMake needed).

## Staged roadmap

1. **Extract the core** (no behavior change, Windows still builds): *in progress* —
   done so far, each build- and launch-verified, `src/main.cpp` down from 3466 → ~2250 lines:
   - `src/core/ui_assets.h` — the entire UI (~1040 lines)
   - `src/core/json.h` — JSON reader/escaper
   - `src/core/models.h` — `Bookmark`/`History`/`Settings` + the search-engine table
   - `src/core/adblock.h` — ad/tracker host list + suffix matcher (`IsAdHost`); the shell's
     `IsAdUrl` now just parses the URL→host (WinHTTP) and calls the portable matcher
   - `src/core/storage.h` — (de)serialization of the data files; the shell keeps only the
     `ReadWideFile`/`WriteWideFile` IO (the `IStorage` seam)
   - `src/core/ai_models.h` — the on-device AI model catalog + lookup
   - `src/core/catalog.h` — the curated extension catalog + `ParseGithubAssetUrl()` (pure
     JSON scan); the shell's resolvers now just fetch the JSON and call the parser

   - `src/core/urls.h` — the address-bar URL-vs-search heuristic + a self-contained UTF-8
     percent-encoder (no dependency on the platform string-conversion API)
   - `src/core/models.h` also now holds `TabModel` — the portable half of a tab; the
     shell's `Tab` just derives from it and adds the native WebView2 handles

   **Portability is verified**, not just asserted: `src/core/selftest.cpp` includes every
   core header with **no windows.h / WebView2 / platform API** and exercises the logic; it
   compiles and passes on a plain C++17 toolchain (`cl /std:c++17 /EHsc src/core/selftest.cpp`,
   or `c++ -std=c++17 …` anywhere). If it builds, the core runs on that platform.

   The platform *services* have also moved to their target home:
   `src/platform/windows/win_services.h` now holds the Windows implementations of the
   `IHttpClient` / `IProcess` / storage-IO seams — file IO, WinHTTP GET/download,
   free-port/health-check sockets, hidden child processes, and manifest discovery.
   Other shells provide the same functions on their own APIs.

   `src/platform/platform.h` defines the target interface. `src/main.cpp` is down from
   3466 → ~1935 lines and is now the Windows *shell proper* — the Win32 window, the
   WebView2 controllers/events, and the app-state orchestration between them. The last
   refactor (optional before starting a second shell) is wrapping the WebView2 calls in
   the `IWebView`/`IWindow` interfaces so the orchestration itself can move into the core.
2. **Linux shell** (`platform/linux/`, WebKitGTK) — closest desktop port; no extensions.
3. **macOS shell** (`platform/macos/`, WKWebView, `.mm`).
4. **Android shell** (`android/`, skeleton already scaffolded) — WebView + JNI bridge
   to the same core compiled with the NDK.

Steps 2–4 reuse 100% of the UI and core; each is a self-contained shell, so they can
land independently without destabilizing Windows.
