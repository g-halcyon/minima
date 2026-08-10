# Minima for Android (shell skeleton)

This is the Android **shell** for Minima — the native container that will host the same
portable C++ core as the desktop builds (see [`../PORTING.md`](../PORTING.md)).

## Status

- ✅ **Runs the real C++ core**: `src/main/cpp/minima_jni.cpp` compiles the repo's
  `src/core/` headers into `libminima_core.so` (all four ABIs) via the NDK — ad/tracker
  matching and address-bar URL-vs-search resolution are the exact same code as the
  desktop builds, reached through `MinimaCore.kt` (with a Kotlin fallback if the native
  lib ever fails to load). Verified on-device: `I/Minima: C++ core loaded: true`.
- ✅ **Minima-styled UI** (emulator-verified, light + dark): the desktop palette and
  design language — rounded pill address bar with the ad-block shield and per-page block
  count, rounded tab counter, card-style full-screen tab switcher with the accent-bordered
  active tab and a "+ New tab" pill, a start page with the gradient "M" logo and bookmark
  tiles, themed status bar, adaptive launcher icon (gradient M).
- ✅ Find-in-page (with match counts), downloads via the system DownloadManager, bookmarks,
  history, session restore, share, http/https intent handling, third-party cookies off.
- ✅ **On-device AI** (emulator-verified): llama.cpp (vendor/llama.cpp) is compiled into
  `libminima_core.so` by the NDK. Typing a query shows a suggestions dropdown under the
  search bar with a "✦ Ask Minima AI" row — tapping it streams a Gemma 3 1B answer into
  an inline card, fully on-device (desktop's signature "fast answer" feature). First use
  downloads the model (~770 MB) via DownloadManager with inline progress; the model
  loads from the internal or external files dir (internal is adb-pushable for dev).
- ✅ Minima-styled overlay panels for the menu, bookmarks, and history (no stock dialogs),
  and a custom-drawn shield (the desktop toolbar's SVG path) with per-page block count.

Build: `gradle assembleDebug` (Gradle 8.9+, AGP 8.7.3 pinned in `settings.gradle`,
NDK + CMake 3.22 from the SDK; `local.properties` must point at your SDK).

## What comes from the core (not reimplemented here)

Once the core is extracted behind `src/platform/platform.h` and compiled with the NDK,
the Activity stops owning browser logic and instead renders the core's chrome HTML and
forwards events. Tabs, bookmarks, history, settings, the setup wizard, the Ctrl-K
command palette, and the on-device AI sidebar are all core features and appear on
Android automatically. **Extensions are hidden on Android** — the system WebView has no
extensions API (`IExtensionManager.supported()` returns false).

## Build

Requires Android Studio (or the command-line SDK) and, for the native core step, the
Android NDK.

```bash
cd android
./gradlew assembleDebug
# → app/build/outputs/apk/debug/app-debug.apk
```

To attach the shared core later, uncomment the `externalNativeBuild` blocks in
`app/build.gradle`; Gradle then invokes the root `CMakeLists.txt` (its `CORE_SOURCES`)
through the NDK and produces `libminima_core.so`, which `MainActivity` loads via JNI.
