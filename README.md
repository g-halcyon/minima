# Minima

An ultra-fast, minimal browser written in **C++ directly on the system webview**
(WebView2 on Windows — Microsoft's Chromium engine, the same one Edge uses). No
frameworks, no runtime layers: one small native executable. Cross-platform work
(macOS/Linux/Android) is under way — see [PORTING.md](PORTING.md).

## Current features

- **Tabs** — Ctrl+T new, Ctrl+W close, middle-click close, live titles, loading spinners;
  popups/`target=_blank` open as tabs
- **Navigation** — address bar with URL/search detection (searches go to Mojeek,
  a private engine with no tracking), back/forward (Alt+←/→), reload (Ctrl+R / F5)
- **Bookmarks + history** — Ctrl+D / Ctrl+H, start-page tiles, address-bar suggestions
- **Find in page** — Ctrl+F bar with match counts and highlight-all (native WebView2 Find)
- **Session restore** — reopens your tabs on startup (toggle in Settings); **downloads** via
  Ctrl+J or the toolbar button
- **Installable, default-browser ready** — single-instance (link clicks open as tabs in the
  running window), "Set as default browser" in Settings, `scripts\install.ps1` for a per-user
  install (Start menu, Default apps, uninstall entry) or `installer\minima.iss` for a setup.exe
- **Ad & tracker blocking** — network-level, per-page block counter on the toolbar shield
- **Extensions** — a curated one-click **catalog** (uBlock Origin, Stylus, Violentmonkey)
  installed straight from their official releases, plus "install unpacked" for any
  Chrome extension (WebView2's browser-extensions API)
- **On-device AI** — a managed `llama-server` sidecar running a small Gemma model, fully
  local. Powers inline answers, a full AI chat page, and the two signature features below.
- **✦ Ask-this-page sidebar** — a docked AI panel that reads the current page and
  summarizes / answers questions about it, on-device — the page never leaves your machine
- **✦ Command palette (Ctrl+K)** — a natural-language bar for commands ("summarize this",
  "new tab", "settings") and one-line AI questions
- **First-run setup wizard** — guided onboarding (search engine, privacy, AI) like a real browser
- **Shortcuts work everywhere** — captured natively even while a page has focus
  (via WebView2 accelerator events)
- **Instant startup** — a bare Win32 window plus WebView2; the chrome UI is a single
  embedded HTML strip, no web framework, system fonts, automatic light/dark theme

## Build

Requires: Windows 10/11 with the WebView2 Runtime (preinstalled on Win 11) and
Visual Studio 2022 Build Tools (C++ workload).

```bat
build.bat        # produces build\minima.exe
build\minima.exe                     # run
build\minima.exe https://example.com # open a URL at startup
```

To install it like a real browser (per-user, no admin):

```powershell
powershell -ExecutionPolicy Bypass -File scripts\install.ps1
```

This copies the exe to `%LOCALAPPDATA%\Programs\Minima`, adds a Start-menu shortcut and
uninstall entry, and registers Minima in Windows **Default apps** so you can choose it for
http/https links. (`installer\minima.iss` builds a distributable setup.exe with Inno Setup.)

The WebView2 SDK lives in `vendor/webview2-pkg` (from NuGet, checked in for
reproducible builds).

## Architecture

`src/main.cpp` (the whole Windows browser; being split into a portable `core/` +
`platform/windows/` per PORTING.md):

- One top-level Win32 window
- A **chrome** WebView2 controller (78 px strip): tabs + address bar as embedded
  HTML/JS, talking to C++ over `postMessage`; C++ pushes tab state back as JSON
- One WebView2 controller **per tab** below the strip; only the active one is visible
- Native events drive everything: `DocumentTitleChanged`, `SourceChanged`,
  `NavigationStarting/Completed` (loading + back/forward state),
  `NewWindowRequested` (popups→tabs), `AcceleratorKeyPressed` (global shortcuts)
- Per-monitor DPI aware; profile data in `%LOCALAPPDATA%\Minima`

## Cross-platform

Minima runs from **one portable C++ core** (models, storage, the entire HTML/JS UI,
adblock, catalogs — `src/core/`, proven platform-free by `src/core/selftest.cpp`) behind
small per-OS shells — no Rust/Tauri, no second copy of Chromium:

| Platform | Shell | Status |
|---|---|---|
| Windows | `src/main.cpp` (Win32 + WebView2) | **Full-featured, shipping** |
| **Android** | `android/` (Kotlin + WebView + **the C++ core and llama.cpp over NDK/JNI**) | **Builds & runs** — Minima-styled UI (light/dark), tabs, private search, adblock, find-in-page, downloads, bookmarks/history, session restore, **on-device AI with the inline answer under the search bar** |
| Linux | `src/platform/linux/shell.cpp` (GTK3 + WebKitGTK) | Code-complete (core browsing, same chrome UI via a `chrome.webview` shim); not yet compiled on Linux |
| macOS | `src/platform/macos/shell.mm` (Cocoa + WKWebView) | Code-complete (core browsing); not yet compiled on macOS |

See **[PORTING.md](PORTING.md)** for the architecture; `cmake -B build -S .` selects the
right desktop shell, `cd android && gradle assembleDebug` builds the APK.

> Note: the curated extension catalog is Windows-only — it needs the Chromium engine's
> extensions API, which the macOS/Linux system webviews (WebKit) don't expose. Ad/tracker
> blocking and everything else are portable.

## Roadmap

1. Compile + polish the Linux/macOS shells on real machines (they are code-complete)
2. Android: AI + NDK/JNI reuse of the C++ core (host list is currently ported by hand)
3. AI sidecar on Linux/macOS; zoom persistence, reader mode, auto-update

The previous Tauri/Rust prototype is archived at `D:\minima-tauri-backup.zip`
(reference for the search scraping + Ollama/llama.cpp integration logic).

## License & Commercial Use

Minima is source-available software licensed under the **[PolyForm Noncommercial License 1.0](LICENSE)**.

- **Non-Commercial Use**: Free for personal, non-commercial, educational, hobby, and research purposes.
- **Commercial Use**: Any commercial use, embedding in paid products, enterprise deployment, or monetization requires a separate **Commercial License** from **[g-halcyon](https://github.com/g-halcyon)**.

See **[LICENSE-COMMERCIAL.md](LICENSE-COMMERCIAL.md)** for details on commercial licensing options and inquiries. Third-party libraries located under `vendor/` (such as `llama.cpp`) retain their respective original open-source licenses (MIT).

