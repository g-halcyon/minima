// platform.h — the boundary between Minima's portable C++ core and the per-OS shell.
//
// The core (models, storage, UI HTML, AI/catalog logic) is written once and talks to
// the outside world ONLY through these interfaces. Each OS provides one implementation:
//   Windows : platform/windows/  (Win32 + WebView2)   — the current src/main.cpp
//   macOS   : platform/macos/    (NSWindow + WKWebView, .mm)
//   Linux   : platform/linux/    (GTK + WebKitGTK)
//   Android : platform/android/  (Activity + WebView, reached over JNI)
//
// This header intentionally uses only std types so it compiles on every toolchain.
// See PORTING.md for the full plan. Nothing in the current Windows build depends on
// this yet; it is the target of the staged core-extraction refactor (roadmap step 1).
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace minima {

// ---- One embedded webview (the chrome strip, a tab, or the AI sidebar) --------------
class IWebView {
public:
    virtual ~IWebView() = default;

    virtual void navigate(const std::string& url) = 0;
    virtual void loadHtml(const std::string& html) = 0;
    virtual void goBack() = 0;
    virtual void goForward() = 0;
    virtual void reload() = 0;

    // Run JS in the page; `onResult` (optional) receives the JSON-encoded return value.
    virtual void eval(const std::string& js,
                      std::function<void(const std::string& jsonResult)> onResult = {}) = 0;

    // Post a JSON message the page reads via its host bridge; the reverse direction is
    // delivered through IWebViewHost::onMessage.
    virtual void postJson(const std::string& json) = 0;

    virtual void setBounds(int x, int y, int w, int h) = 0;
    virtual void setVisible(bool visible) = 0;
    virtual void setZoom(double factor) = 0;
    virtual void setMuted(bool muted) = 0;
};

// Events a webview raises back into the core (implemented by the core, called by shell).
class IWebViewHost {
public:
    virtual ~IWebViewHost() = default;
    virtual void onTitleChanged(const std::string& title) = 0;
    virtual void onUrlChanged(const std::string& url) = 0;
    virtual void onFaviconChanged(const std::string& url) = 0;
    virtual void onLoadingChanged(bool loading, bool canBack, bool canFwd) = 0;
    virtual void onMessage(const std::string& text) = 0;         // page → core bridge
    virtual void onNewWindow(const std::string& url) = 0;        // popups / target=_blank
    virtual bool onAccelerator(int vkey, bool ctrl, bool alt, bool shift) = 0; // return handled
    // Return true to block the request (ad/tracker filtering lives in the core).
    virtual bool onResourceRequest(const std::string& url) = 0;
};

// ---- The native top-level window ----------------------------------------------------
class IWindow {
public:
    virtual ~IWindow() = default;
    virtual std::unique_ptr<IWebView> createWebView(IWebViewHost* host, bool supportsExtensions) = 0;
    virtual void setTitle(const std::string& title) = 0;
    virtual void clientSize(int& w, int& h) const = 0;
    virtual float dpiScale() const = 0;
    virtual void toggleFullscreen() = 0;
};

// ---- Platform services the core needs (no Win32/WinHTTP leaking into the core) ------
struct HttpResponse { int status = 0; std::string body; };

class IHttpClient {
public:
    virtual ~IHttpClient() = default;
    virtual HttpResponse get(const std::string& url) = 0;
    // Streams to `destPath`; `onProgress(done,total)` may be called repeatedly.
    virtual bool download(const std::string& url, const std::string& destPath,
                          std::function<void(int64_t done, int64_t total)> onProgress = {}) = 0;
};

// Launch/manage the on-device AI sidecar (llama-server) and unzip downloads.
class IProcess {
public:
    virtual ~IProcess() = default;
    virtual bool start(const std::string& exe, const std::vector<std::string>& args) = 0;
    virtual void terminate() = 0;
    virtual bool extractZip(const std::string& zipPath, const std::string& destDir) = 0;
};

// Extension management — a no-op backend on platforms without a Chromium engine.
class IExtensionManager {
public:
    virtual ~IExtensionManager() = default;
    virtual bool supported() const = 0;
    virtual void addUnpacked(const std::string& folder, std::function<void(bool ok)> done) = 0;
    struct Ext { std::string id, name; bool enabled; };
    virtual void list(std::function<void(std::vector<Ext>)> done) = 0;
    virtual void setEnabled(const std::string& id, bool enabled) = 0;
    virtual void remove(const std::string& id) = 0;
};

// Bundle handed to the core at startup by each shell's entry point.
struct Platform {
    IWindow* window;
    IHttpClient* http;
    IProcess* process;
    IExtensionManager* extensions;
    std::string profileDir;   // writable per-user data directory
    std::function<void(std::function<void()>)> runOnUiThread; // marshal a callback to the UI thread
};

// Implemented by the core; each shell calls this after building its Platform.
void RunMinima(const Platform& platform, const std::string& initialUrl);

} // namespace minima
