// Minima — a minimal, fast browser built directly on WebView2 (C++/Win32).
// Copyright (c) 2026 g-halcyon (https://github.com/g-halcyon)
// Licensed under PolyForm Noncommercial License 1.0 (PolyForm-Noncommercial-1.0).
// Commercial use requires a commercial license agreement from g-halcyon.
//
// One top-level window hosts:
//   * a "chrome" WebView2 (tabs + address bar, embedded HTML below)
//   * one WebView2 controller per tab, laid out under the chrome strip
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <dwmapi.h>
#include <wrl.h>
#include <winhttp.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <string>
#include <vector>
#include <sstream>
#include <algorithm>
#include <memory>
#include <ctime>
#include <atomic>
#include "WebView2.h"
#include "WebView2EnvironmentOptions.h"
#include "core/json.h"
#include "core/models.h"
#include "core/adblock.h"
#include "core/storage.h"
#include "core/ai_models.h"
#include "core/catalog.h"
#include "core/urls.h"
#include "platform/windows/win_services.h"
#include "platform/windows/win_register.h"

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;
using namespace minima; // portable core helpers (JSON, …)

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "version.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "dwmapi.lib")

static const int kChromeHeightDip = 88;
static const int kSuggestMaxDip = 340;

/// RAII wrapper for COM-allocated strings (replaces the WIL dependency).
struct CoStr {
    LPWSTR p = nullptr;
    ~CoStr() {
        if (p) CoTaskMemFree(p);
    }
    LPWSTR* operator&() { return &p; }
    explicit operator bool() const { return p != nullptr; }
    const wchar_t* get() const { return p; }
};

#include "core/ui_assets.h"

// ---------------------------------------------------------------------------

// The Windows shell's tab = the portable TabModel (core/models.h) + the native WebView2
// handles. Deriving keeps every `t->title`/`t->url`/`t->isAiPage` reference unchanged.
struct Tab : minima::TabModel {
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> webview;
};

struct App {
    HWND hwnd = nullptr;
    ComPtr<ICoreWebView2Environment> env;
    ComPtr<ICoreWebView2Controller> chromeController;
    ComPtr<ICoreWebView2> chromeWebview;
    ComPtr<ICoreWebView2Controller> sidebarController; // "Ask this page" AI panel (docked right)
    ComPtr<ICoreWebView2> sidebarWebview;
    bool sidebarOpen = false;
    ComPtr<ICoreWebView2Profile> profile;
    std::vector<std::unique_ptr<Tab>> tabs;
    std::vector<std::wstring> closedUrls; // for Ctrl+Shift+T
    std::wstring activeId;
    std::wstring initialUrl; // optional URL from the command line
    int nextTabNum = 1;
    int suggestDip = 0;         // extra chrome height for the suggestion dropdown / AI answer
    bool htmlFullscreen = false; // a page element (e.g. video) went fullscreen
};

static App g_app;
static bool g_firstRun = false; // no settings.json existed at startup → show the setup wizard

static int ChromeHeightPx() {
    if (g_app.htmlFullscreen) return 0;
    UINT dpi = GetDpiForWindow(g_app.hwnd);
    int dip = kChromeHeightDip + std::min(g_app.suggestDip, kSuggestMaxDip);
    return MulDiv(dip, dpi, 96);
}

static Tab* FindTab(const std::wstring& id) {
    for (auto& t : g_app.tabs)
        if (t->id == id) return t.get();
    return nullptr;
}

static Tab* ActiveTab() { return FindTab(g_app.activeId); }

static void Relayout() {
    if (!g_app.hwnd) return;
    RECT rc;
    GetClientRect(g_app.hwnd, &rc);
    int chromeH = ChromeHeightPx();
    if (g_app.chromeController) {
        RECT b{0, 0, rc.right, chromeH};
        g_app.chromeController->put_Bounds(b);
        g_app.chromeController->put_IsVisible(g_app.htmlFullscreen ? FALSE : TRUE);
    }
    bool sbOn = g_app.sidebarOpen && !g_app.htmlFullscreen && g_app.sidebarController;
    int sbW = 0;
    if (sbOn) {
        UINT dpi = GetDpiForWindow(g_app.hwnd);
        sbW = std::min<LONG>(MulDiv(390, dpi, 96), rc.right / 2);
    }
    if (g_app.sidebarController) {
        RECT b{rc.right - sbW, chromeH, rc.right, rc.bottom};
        g_app.sidebarController->put_Bounds(b);
        g_app.sidebarController->put_IsVisible(sbOn ? TRUE : FALSE);
    }
    for (auto& t : g_app.tabs) {
        if (!t->controller) continue;
        if (t->id == g_app.activeId) {
            RECT b{0, chromeH, rc.right - sbW, rc.bottom};
            t->controller->put_Bounds(b);
            t->controller->put_IsVisible(TRUE);
        } else {
            t->controller->put_IsVisible(FALSE);
        }
    }
}

static bool IsBookmarked(const std::wstring& url);
static bool IsAdblockOn();

// AI setup phases: 0 idle, 1 downloading engine, 2 extracting, 3 downloading model,
// 4 starting server, 5 ready, 6 error.
struct AiState {
    std::atomic<int> phase{0};
    std::atomic<long long> downloaded{0};
    std::atomic<long long> total{0};
    std::atomic<int> port{0};
    std::atomic<bool> serverStarted{false};
    std::wstring errorMsg;
    PROCESS_INFORMATION serverProc{};
};
static AiState g_ai;
static std::wstring g_aiTabId;

/// Push the full tab/address state to the chrome UI.
static void ScheduleSaveSession();

static void SyncState() {
    ScheduleSaveSession(); // tab set/active/url changed — snapshot (debounced)
    if (!g_app.chromeWebview) return;
    std::wstringstream ss;
    ss << L"{\"active\":\"" << JsonEscape(g_app.activeId) << L"\",\"aiPort\":" << g_ai.port.load()
       << L",\"aiPhase\":" << g_ai.phase.load()
       << L",\"adblock\":" << (IsAdblockOn() ? L"true" : L"false") << L",\"tabs\":[";
    bool first = true;
    for (auto& t : g_app.tabs) {
        if (!first) ss << L",";
        first = false;
        std::wstring url = t->url.rfind(L"data:", 0) == 0 ? L"" : t->url;
        ss << L"{\"id\":\"" << JsonEscape(t->id) << L"\",\"title\":\"" << JsonEscape(t->title)
           << L"\",\"url\":\"" << JsonEscape(url) << L"\",\"fav\":\"" << JsonEscape(t->favicon)
           << L"\",\"blocked\":" << t->blocked
           << L",\"audio\":" << (t->audio ? L"true" : L"false")
           << L",\"muted\":" << (t->muted ? L"true" : L"false")
           << L",\"loading\":" << (t->loading ? L"true" : L"false")
           << L",\"canBack\":" << (t->canBack ? L"true" : L"false")
           << L",\"canFwd\":" << (t->canFwd ? L"true" : L"false")
           << L",\"bookmarked\":" << (IsBookmarked(t->url) ? L"true" : L"false") << L"}";
    }
    ss << L"]}";
    g_app.chromeWebview->PostWebMessageAsJson(ss.str().c_str());
    if (Tab* t = ActiveTab()) {
        std::wstring title = t->title.empty() ? L"Minima" : t->title + L" — Minima";
        SetWindowTextW(g_app.hwnd, title.c_str());
    }
}

/// Coalesces bursts of state changes (favicon, blocked counters) into one SyncState.
static const UINT_PTR kSyncTimerId = 2;
static void ScheduleSync() {
    if (g_app.hwnd) SetTimer(g_app.hwnd, kSyncTimerId, 200, nullptr);
}

// ---------------------------------------------------------------------------
// Bookmarks + history: flat JSON arrays persisted in %LOCALAPPDATA%\Minima.
// Files are our own UTF-16LE dumps (binary read/write), so no encoding
// conversion is needed — only this app ever reads them.
// ---------------------------------------------------------------------------


static std::wstring g_dataDir;
static std::vector<BookmarkEntry> g_bookmarks;
static std::vector<HistoryEntry> g_history;
static const size_t kMaxHistory = 5000;

static void LoadBookmarks() {
    g_bookmarks = ParseBookmarks(ReadWideFile(g_dataDir + L"\\bookmarks.json"));
}

static void SaveBookmarks() {
    WriteWideFile(g_dataDir + L"\\bookmarks.json", SerializeBookmarks(g_bookmarks));
}

static bool IsBookmarked(const std::wstring& url) {
    for (auto& b : g_bookmarks)
        if (b.url == url) return true;
    return false;
}

/// Adds or removes a bookmark for `url`; no-op for internal/blank pages.
static void ToggleBookmark(const std::wstring& url, const std::wstring& title) {
    if (url.empty() || url.rfind(L"about:", 0) == 0 || url.rfind(L"data:", 0) == 0) return;
    auto it = std::find_if(g_bookmarks.begin(), g_bookmarks.end(),
                           [&](const auto& b) { return b.url == url; });
    if (it != g_bookmarks.end()) {
        g_bookmarks.erase(it);
    } else {
        g_bookmarks.push_back({url, title.empty() ? url : title});
    }
    SaveBookmarks();
}

static void LoadHistory() {
    g_history = ParseHistory(ReadWideFile(g_dataDir + L"\\history.json"));
}

static void SaveHistory() {
    WriteWideFile(g_dataDir + L"\\history.json", SerializeHistory(g_history));
}

/// Records a successful navigation; skips internal/blank pages, caps total size.
/// Repeat visits move the existing entry to the top instead of duplicating it.
static void AddHistory(const std::wstring& url, const std::wstring& title) {
    if (url.empty() || url.rfind(L"about:", 0) == 0 || url.rfind(L"data:", 0) == 0) return;
    auto it = std::find_if(g_history.begin(), g_history.end(),
                           [&](const auto& h) { return h.url == url; });
    if (it != g_history.end()) g_history.erase(it);
    g_history.insert(g_history.begin(), {url, title, static_cast<long long>(time(nullptr))});
    if (g_history.size() > kMaxHistory) g_history.resize(kMaxHistory);
    SaveHistory();
}

/// Formats a unix timestamp as "YYYY-MM-DD HH:MM" in local time.
static std::wstring FormatHistoryTime(long long unixTime) {
    time_t t = static_cast<time_t>(unixTime);
    tm local{};
    if (localtime_s(&local, &t) != 0) return L"";
    wchar_t buf[32];
    swprintf(buf, 32, L"%04d-%02d-%02d %02d:%02d", local.tm_year + 1900, local.tm_mon + 1,
             local.tm_mday, local.tm_hour, local.tm_min);
    return buf;
}

// ---------------------------------------------------------------------------
// Settings: search engine + AI model choice, persisted as a flat JSON object.
// ---------------------------------------------------------------------------

static Settings g_settings;

static bool IsAdblockOn() { return g_settings.adblock; }

static const SearchEngine& CurrentSearchEngine() { return FindSearchEngine(g_settings.searchEngine); }

static void LoadSettings() {
    ApplySettings(g_settings, ReadWideFile(g_dataDir + L"\\settings.json"));
}

static void SaveSettings() {
    WriteWideFile(g_dataDir + L"\\settings.json", SerializeSettings(g_settings));
}

// ---------------------------------------------------------------------------
// Session restore: the open tabs are saved (debounced) as session.json and
// reopened on the next startup when the "restore session" setting is on.
// ---------------------------------------------------------------------------
static const UINT_PTR kSessionTimerId = 3;

/// Snapshots the current tabs to session.json (internal pages are skipped).
static void SaveSession() {
    std::vector<SessionTab> tabs;
    for (auto& t : g_app.tabs) {
        if (t->url.empty() || t->url.rfind(L"about:", 0) == 0 || t->url.rfind(L"data:", 0) == 0)
            continue;
        tabs.push_back({t->url, t->id == g_app.activeId});
    }
    WriteWideFile(g_dataDir + L"\\session.json", SerializeSession(tabs));
}

/// Debounces session writes — tab state changes in bursts (navigation, closes).
static void ScheduleSaveSession() {
    if (g_app.hwnd) SetTimer(g_app.hwnd, kSessionTimerId, 2000, nullptr);
}

// ---------------------------------------------------------------------------
// Built-in ad & tracker blocking. The host list + suffix matcher are portable
// (core/adblock.h); only the URL→host parsing below is platform-specific (WinHTTP).
// ---------------------------------------------------------------------------
static bool IsAdUrl(const wchar_t* url) {
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256]{};
    uc.lpszHostName = host;
    uc.dwHostNameLength = 255;
    if (!WinHttpCrackUrl(url, 0, 0, &uc) || uc.dwHostNameLength == 0) return false;
    return IsAdHost(std::wstring(host, uc.dwHostNameLength));
}

static std::wstring BuildStartHtml() {
    std::wstringstream tiles;
    for (auto& b : g_bookmarks) {
        std::wstring label = b.title.empty() ? b.url : b.title;
        wchar_t initial = label.empty() ? L'?' : towupper(label[0]);
        tiles << L"<a class='tile' href='" << JsonEscape(b.url) << L"' title='" << JsonEscape(b.url) << L"'>"
              << L"<div class='fav'>" << initial << L"</div><span>" << JsonEscape(label) << L"</span></a>";
    }
    return kStartHtmlHead + tiles.str() + kStartHtmlTail;
}

static std::wstring BuildHistoryHtml() {
    std::wstringstream rows;
    if (g_history.empty()) rows << L"<p class='empty'>No history yet.</p>";
    for (auto& h : g_history) {
        rows << L"<a class='row' href='" << JsonEscape(h.url) << L"'><span class='t'>"
             << JsonEscape(h.title.empty() ? h.url : h.title) << L"</span><span class='u'>"
             << JsonEscape(h.url) << L" &mdash; " << FormatHistoryTime(h.time) << L"</span></a>";
    }
    return kHistoryHtmlHead + rows.str() + kHistoryHtmlTail;
}

// ---------------------------------------------------------------------------
// First-run setup wizard — a guided onboarding shown the first time Minima
// starts, like any real browser. Renders as an internal page; step navigation
// happens in JS, and each choice is posted to C++ as it's made.
// ---------------------------------------------------------------------------
static std::wstring BuildWizardHtml() {
    std::wstringstream engines;
    for (auto& e : kSearchEngines) {
        bool sel = g_settings.searchEngine == e.id;
        engines << L"<label class='opt'><input type='radio' name='se' value='" << e.id << L"'"
                << (sel ? L" checked" : L"") << L"><span>" << e.label << L"</span></label>";
    }
    std::wstringstream html;
    html << LR"HTML(<!doctype html>
<html><head><meta charset="utf-8"><title>Welcome to Minima</title><style>
  :root { color-scheme: light dark; }
  * { box-sizing:border-box; margin:0; padding:0; }
  body { font:15px/1.55 "Segoe UI",system-ui; height:100vh; display:flex; align-items:center;
         justify-content:center; background:#f6f7f9; color:#1a1a1a; overflow:hidden; }
  @media (prefers-color-scheme: dark) { body { background:#131317; color:#eee; } }
  .card { width:520px; max-width:92vw; background:#fff; border-radius:20px; padding:40px 44px 32px;
          box-shadow:0 18px 60px rgba(0,0,0,.13); }
  @media (prefers-color-scheme: dark) { .card { background:#1d1e22; box-shadow:0 18px 60px rgba(0,0,0,.5); } }
  .logo { width:60px; height:60px; border-radius:18px; margin:0 auto 20px; display:flex; align-items:center;
          justify-content:center; font-size:30px; color:#fff; font-weight:800;
          background:linear-gradient(135deg,#4a80f5,#7c5cff); box-shadow:0 8px 24px rgba(74,128,245,.4); }
  h1 { font-size:23px; font-weight:700; text-align:center; margin-bottom:8px; }
  .sub { text-align:center; color:#888; font-size:14px; margin-bottom:26px; }
  .step { display:none; }
  .step.on { display:block; animation:fade .25s ease; }
  @keyframes fade { from { opacity:0; transform:translateY(6px); } to { opacity:1; transform:none; } }
  .opt { display:flex; align-items:center; gap:12px; padding:12px 14px; border-radius:11px; cursor:pointer;
         border:1.5px solid transparent; background:rgba(0,0,0,.035); margin-bottom:8px; font-size:14px; }
  @media (prefers-color-scheme: dark) { .opt { background:rgba(255,255,255,.05); } }
  .opt:hover { background:rgba(74,128,245,.09); }
  .opt input { accent-color:#4a80f5; width:17px; height:17px; }
  .feat { display:flex; gap:14px; padding:14px 4px; }
  .feat .em { font-size:22px; }
  .feat .ft { font-weight:600; font-size:14.5px; }
  .feat .fd { color:#888; font-size:13px; }
  .toggle { display:flex; align-items:center; justify-content:space-between; padding:14px 16px;
            border-radius:12px; background:rgba(0,0,0,.035); font-size:14px; }
  @media (prefers-color-scheme: dark) { .toggle { background:rgba(255,255,255,.05); } }
  .toggle .d { color:#888; font-size:12.5px; margin-top:2px; }
  .sw { position:relative; width:46px; height:26px; flex:none; }
  .sw input { opacity:0; width:0; height:0; }
  .sw .tr { position:absolute; inset:0; background:#c8ccd2; border-radius:20px; transition:.2s; cursor:pointer; }
  .sw .tr:before { content:''; position:absolute; width:20px; height:20px; left:3px; top:3px; background:#fff;
                   border-radius:50%; transition:.2s; }
  .sw input:checked + .tr { background:#4a80f5; }
  .sw input:checked + .tr:before { transform:translateX(20px); }
  .nav { display:flex; align-items:center; justify-content:space-between; margin-top:28px; }
  .dots { display:flex; gap:7px; }
  .dots i { width:7px; height:7px; border-radius:50%; background:rgba(0,0,0,.15); transition:.2s; }
  @media (prefers-color-scheme: dark) { .dots i { background:rgba(255,255,255,.2); } }
  .dots i.on { background:#4a80f5; width:20px; border-radius:4px; }
  button { border:none; border-radius:10px; padding:11px 22px; font:14px "Segoe UI",system-ui; font-weight:600;
           cursor:pointer; }
  .primary { background:#4a80f5; color:#fff; }
  .primary:hover { background:#3a6fe0; }
  .ghost { background:none; color:#888; }
  .ghost:hover { color:#4a80f5; }
  #aiNote { font-size:12.5px; color:#4a80f5; text-align:center; margin-top:12px; min-height:16px; }
</style></head><body>
  <div class="card">
    <div class="logo">M</div>
    <div class="step on" data-step="0">
      <h1>Welcome to Minima</h1>
      <p class="sub">A fast, private, minimal browser. Let's set it up &mdash; takes 20 seconds.</p>
      <div class="feat"><span class="em">&#9889;</span><div><div class="ft">Instant &amp; lightweight</div><div class="fd">A tiny native browser on the same engine as Edge.</div></div></div>
      <div class="feat"><span class="em">&#128274;</span><div><div class="ft">Private by default</div><div class="fd">Built-in ad &amp; tracker blocking, private search.</div></div></div>
      <div class="feat"><span class="em">&#10022;</span><div><div class="ft">On-device AI</div><div class="fd">Ask questions and summarize pages &mdash; nothing leaves your device.</div></div></div>
    </div>
    <div class="step" data-step="1">
      <h1>Choose your search engine</h1>
      <p class="sub">You can change this anytime in Settings.</p>
      <div id="engines">)HTML"
         << engines.str() << LR"HTML(</div>
    </div>
    <div class="step" data-step="2">
      <h1>Privacy protection</h1>
      <p class="sub">Minima can block ads and trackers on every page.</p>
      <label class="toggle"><span>Block ads &amp; trackers<div class="d">Recommended &mdash; faster, cleaner, more private browsing.</div></span>
        <span class="sw"><input type="checkbox" id="adblock" checked><span class="tr"></span></span></label>
    </div>
    <div class="step" data-step="3">
      <h1>On-device AI</h1>
      <p class="sub">Optional. A small Gemma model runs fully on your device.</p>
      <div class="feat"><span class="em">&#129504;</span><div><div class="ft">Set it up now</div><div class="fd">Downloads the engine (~60&nbsp;MB) and model (~800&nbsp;MB) in the background.</div></div></div>
      <label class="toggle"><span>Download AI in the background<div class="d">You can also do this later from the AI page.</div></span>
        <span class="sw"><input type="checkbox" id="aiopt"><span class="tr"></span></span></label>
      <div id="aiNote"></div>
    </div>
    <div class="step" data-step="4">
      <h1>You're all set &#127881;</h1>
      <p class="sub">Minima is ready. Tips: <b>Ctrl+K</b> opens the AI command palette, and the
        sparkle button asks AI about the page you're on.</p>
    </div>
    <div class="nav">
      <button class="ghost" id="back" style="visibility:hidden">Back</button>
      <div class="dots"><i class="on"></i><i></i><i></i><i></i><i></i></div>
      <button class="primary" id="next">Next</button>
    </div>
  </div>
<script>
  const send = (cmd, arg='') => window.chrome.webview.postMessage(cmd + '\x1F' + arg);
  let step = 0; const last = 4;
  const steps = document.querySelectorAll('.step');
  const dots = document.querySelectorAll('.dots i');
  const backBtn = document.getElementById('back');
  const nextBtn = document.getElementById('next');
  function show() {
    steps.forEach((s) => s.classList.toggle('on', +s.dataset.step === step));
    dots.forEach((d, i) => d.classList.toggle('on', i === step));
    backBtn.style.visibility = step === 0 ? 'hidden' : 'visible';
    nextBtn.textContent = step === last ? 'Start browsing' : 'Next';
  }
  backBtn.onclick = () => { if (step > 0) { step--; show(); } };
  nextBtn.onclick = () => {
    if (step === 1) send('wiz-search', document.querySelector('input[name=se]:checked').value);
    if (step === 2) send('wiz-adblock', document.getElementById('adblock').checked ? '1' : '0');
    if (step === 3 && document.getElementById('aiopt').checked) send('wiz-ai');
    if (step === last) { send('wiz-done'); return; }
    step++; show();
  };
  document.getElementById('engines').addEventListener('change', (e) => {
    if (e.target.name === 'se') send('wiz-search', e.target.value);
  });
  document.getElementById('aiopt').addEventListener('change', (e) => {
    document.getElementById('aiNote').textContent = e.target.checked
      ? 'AI will start downloading when you finish setup.' : '';
  });
</script></body></html>)HTML";
    return html.str();
}

// ---------------------------------------------------------------------------
// Local AI: a llama-server sidecar running a small on-device Gemma model.
// The engine binary and model file are fetched on first use into
// %LOCALAPPDATA%\Minima\bin and \models; the server binds a free localhost
// port (probed, not fixed) so it never collides with other local dev servers,
// and it's only alive while Minima is running.
// (AiState/g_ai declared earlier, near the top, so SyncState can report AI readiness.)
// ---------------------------------------------------------------------------

static const AiModelOption& CurrentAiModel() { return FindAiModel(g_settings.aiModel); }

static std::wstring BinDir() { return g_dataDir + L"\\bin"; }
static std::wstring ModelsDir() { return g_dataDir + L"\\models"; }
static std::wstring EnginePath() { return BinDir() + L"\\llama-server.exe"; }
static std::wstring ModelPath() { return ModelsDir() + L"\\" + CurrentAiModel().file; }

/// Looks up the latest llama.cpp release and returns the plain CPU Windows x64 build's zip URL.
/// On failure, diagErr/diagLen (if given) receive a Win32/WinHTTP error code and response length for debugging.
static std::wstring FindLlamaServerZipUrl(DWORD* diagErr = nullptr, size_t* diagLen = nullptr,
                                          std::wstring* diagBody = nullptr) {
    DWORD err = 0;
    std::string json = HttpGetText(L"api.github.com", L"/repos/ggml-org/llama.cpp/releases/latest", &err);
    if (diagErr) *diagErr = err;
    if (diagLen) *diagLen = json.size();
    if (diagBody) *diagBody = Utf8ToWide(json.substr(0, 200));
    return Utf8ToWide(ParseGithubAssetUrl(json, "-bin-win-cpu-x64.zip"));
}

static DWORD WINAPI AiSetupThreadProc(LPVOID) {
    CreateDirectoryW(BinDir().c_str(), nullptr);
    CreateDirectoryW(ModelsDir().c_str(), nullptr);

    if (!FileExists(EnginePath())) {
        g_ai.phase = 1;
        g_ai.downloaded = 0;
        g_ai.total = 0;
        DWORD diagErr = 0;
        size_t diagLen = 0;
        std::wstring diagBody;
        std::wstring zipUrl = FindLlamaServerZipUrl(&diagErr, &diagLen, &diagBody);
        if (zipUrl.empty()) {
            if (diagBody.find(L"rate limit") != std::wstring::npos) {
                g_ai.errorMsg = L"GitHub is rate-limiting this network right now. Wait a bit and try again.";
            } else if (diagErr != 0) {
                wchar_t buf[128];
                swprintf(buf, 128, L"Could not reach the download server (network error %lu).", diagErr);
                g_ai.errorMsg = buf;
            } else {
                g_ai.errorMsg = L"Could not find a llama.cpp release to download.";
            }
            g_ai.phase = 6;
            return 0;
        }
        std::wstring zipPath = BinDir() + L"\\engine.zip";
        if (!DownloadFile(zipUrl, zipPath, &g_ai.downloaded, &g_ai.total)) {
            g_ai.errorMsg = L"Engine download failed.";
            g_ai.phase = 6;
            return 0;
        }
        g_ai.phase = 2;
        RunAndWait(L"tar.exe -xf \"" + zipPath + L"\" -C \"" + BinDir() + L"\"", 60000);
        DeleteFileW(zipPath.c_str());
        if (!FileExists(EnginePath())) {
            g_ai.errorMsg = L"Engine extraction failed.";
            g_ai.phase = 6;
            return 0;
        }
    }

    if (!FileExists(ModelPath())) {
        g_ai.phase = 3;
        g_ai.downloaded = 0;
        g_ai.total = 0;
        if (!DownloadFile(CurrentAiModel().url, ModelPath(), &g_ai.downloaded, &g_ai.total)) {
            g_ai.errorMsg = L"Model download failed.";
            g_ai.phase = 6;
            return 0;
        }
    }

    g_ai.phase = 4;
    int port = FindFreePort();
    std::wstring cmd = L"\"" + EnginePath() + L"\" -m \"" + ModelPath() + L"\" --port " +
                       std::to_wstring(port) + L" --host 127.0.0.1 -c 4096 -ngl 0";
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si,
                        &g_ai.serverProc)) {
        g_ai.errorMsg = L"Failed to start the AI engine.";
        g_ai.phase = 6;
        return 0;
    }
    g_ai.serverStarted = true;

    // Poll for real readiness (model loaded, not just the port accepting connections) — up to ~2 minutes.
    for (int i = 0; i < 600; i++) {
        Sleep(200);
        if (CheckLocalHealth(port)) break;
    }
    g_ai.port = port;
    g_ai.phase = 5;
    return 0;
}

static const UINT_PTR kAiTimerId = 1;

static void StartAiSetup() {
    int p = g_ai.phase.load();
    if (p != 0 && p != 6) return; // already running or ready
    g_ai.phase = 1;
    g_ai.errorMsg.clear();
    HANDLE h = CreateThread(nullptr, 0, AiSetupThreadProc, nullptr, 0, nullptr);
    if (h) CloseHandle(h);
    if (g_app.hwnd) SetTimer(g_app.hwnd, kAiTimerId, 400, nullptr);
}

static std::wstring BuildAiHtml(const std::wstring& initialQuery) {
    std::wstringstream html;
    html << kAiHtmlHead;
    html << L"let port = " << g_ai.port.load() << L";\n";
    html << L"let initialQuery = \"" << JsonEscape(initialQuery) << L"\";\n";
    html << L"let initPhase = " << g_ai.phase.load() << L", initDl = " << g_ai.downloaded.load()
         << L", initTotal = " << g_ai.total.load() << L";\n";
    html << kAiHtmlTail;
    return html.str();
}

/// Pushes the current setup phase/progress into the open AI tab, if any.
static void PushAiStatus() {
    SyncState(); // keep the chrome UI's knowledge of aiPort current too
    if (g_aiTabId.empty()) return;
    Tab* t = FindTab(g_aiTabId);
    if (!t || !t->webview || !t->isAiPage) return;
    std::wstringstream js;
    js << L"window.__minimaAiStatus && window.__minimaAiStatus(" << g_ai.phase.load() << L","
       << g_ai.downloaded.load() << L"," << g_ai.total.load() << L"," << g_ai.port.load() << L",\""
       << JsonEscape(g_ai.errorMsg) << L"\")";
    t->webview->ExecuteScript(js.str().c_str(), nullptr);
}

enum class PageKind { Normal, Ai, Settings, Wizard };

static void NavigateInput(Tab* tab, std::wstring input);
static void CreateTab(const std::wstring& navigateTo, const std::wstring& htmlContent = L"",
                      PageKind kind = PageKind::Normal);
static void CloseTab(const std::wstring& id);
static void RefreshSidebar(); // re-push page context/state to the AI sidebar if open
static void OpenFindBar();    // Ctrl+F find bar in the chrome UI

/// Stops the running AI server (if any) and resets setup state, e.g. before switching models.
static void ResetAiServer() {
    if (g_ai.serverStarted.load() && g_ai.serverProc.hProcess) {
        TerminateProcess(g_ai.serverProc.hProcess, 0);
        CloseHandle(g_ai.serverProc.hProcess);
        CloseHandle(g_ai.serverProc.hThread);
        g_ai.serverProc = PROCESS_INFORMATION{};
        g_ai.serverStarted = false;
    }
    g_ai.phase = 0;
    g_ai.port = 0;
    g_ai.downloaded = 0;
    g_ai.total = 0;
    g_ai.errorMsg.clear();
}

static void SwitchAiModel(const std::wstring& id) {
    if (g_settings.aiModel == id) return;
    int phase = g_ai.phase.load();
    if (phase >= 1 && phase <= 4) return; // ignore while a setup is already in flight
    ResetAiServer();
    g_settings.aiModel = id;
    SaveSettings();
    SyncState();
    if (FileExists(EnginePath()) && FileExists(ModelPath())) StartAiSetup();
}

struct ExtInfo {
    std::wstring id, name;
    bool enabled;
};

// ---------------------------------------------------------------------------
// Curated extension catalog — one-click install of well-known extensions,
// pulled from their official GitHub releases (unpacked Chromium builds), then
// handed to WebView2's AddBrowserExtension. No Chrome Web Store scraping. The
// install machinery (thread, download, extract) lives further below.
// ---------------------------------------------------------------------------
static std::atomic<bool> g_extBusy{false};       // an install is downloading/extracting
static std::wstring g_extBusyName;               // name being installed (for the settings UI)
static const UINT WM_APP_EXT_DONE = WM_APP + 1;  // lParam = new std::pair<bool,std::wstring>*

static std::wstring BuildSettingsHtml(const std::vector<ExtInfo>& exts, bool extSupported) {
    std::wstringstream engines, models;
    for (auto& e : kSearchEngines) {
        bool sel = g_settings.searchEngine == e.id;
        engines << L"<label class='row'><input type='radio' name='se' value='" << e.id << L"'"
                << (sel ? L" checked" : L"") << L"><span>" << e.label << L"</span></label>";
    }
    bool setupBusy = g_ai.phase.load() >= 1 && g_ai.phase.load() <= 4;
    for (auto& m : kAiModels) {
        bool sel = g_settings.aiModel == m.id;
        bool have = FileExists(ModelsDir() + L"\\" + m.file);
        models << L"<label class='row'" << (setupBusy ? L" style='opacity:.5'" : L"") << L"><input type='radio' name='am' value='"
               << m.id << L"'" << (sel ? L" checked" : L"") << (setupBusy ? L" disabled" : L"") << L"><span>" << m.label
               << (have ? L" &nbsp;<i class='badge'>downloaded</i>" : L"") << L"</span></label>";
    }
    std::wstringstream html;
    html << LR"HTML(<!doctype html>
<html><head><meta charset="utf-8"><title>Settings</title><style>
  :root { color-scheme: light dark; }
  * { box-sizing:border-box; }
  html, body { overflow-x:hidden; }
  body { margin:0; font:14px/1.5 "Segoe UI",system-ui; padding:32px 15% 60px; background:#fafafa; color:#1a1a1a; }
  @media (prefers-color-scheme: dark) { body { background:#16161a; color:#eee; } }
  h1 { font-size:22px; margin:0 0 28px; }
  h2 { font-size:14px; margin:0 0 12px; color:#888; text-transform:uppercase; letter-spacing:.03em; }
  section { margin-bottom:32px; }
  .row { display:flex; align-items:center; gap:10px; padding:9px 10px; border-radius:8px; cursor:pointer; font-size:13.5px; }
  .row:hover { background:rgba(0,0,0,.05); }
  @media (prefers-color-scheme: dark) { .row:hover { background:rgba(255,255,255,.07); } }
  .row input { accent-color:#4a80f5; }
  .badge { font-style:normal; font-size:11px; color:#2c9a5c; background:rgba(44,154,92,.12); padding:1px 7px;
           border-radius:8px; margin-left:2px; }
  .actions { display:flex; gap:10px; flex-wrap:wrap; }
  button.btn { background:rgba(0,0,0,.06); color:inherit; border:none; border-radius:8px; padding:9px 16px;
               font:13px "Segoe UI",system-ui; font-weight:600; cursor:pointer; }
  @media (prefers-color-scheme: dark) { button.btn { background:rgba(255,255,255,.09); } }
  button.btn:hover { background:rgba(0,0,0,.1); }
  @media (prefers-color-scheme: dark) { button.btn:hover { background:rgba(255,255,255,.14); } }
  button.btn.danger { color:#e0555f; }
  #note { font-size:12.5px; color:#888; margin-top:10px; min-height:16px; }
  .switch { display:flex; align-items:center; justify-content:space-between; padding:9px 10px;
            border-radius:8px; font-size:13.5px; cursor:pointer; }
  .switch:hover { background:rgba(0,0,0,.05); }
  @media (prefers-color-scheme: dark) { .switch:hover { background:rgba(255,255,255,.07); } }
  .switch .sub { font-size:12px; color:#888; margin-top:1px; }
  .extrow { display:flex; align-items:center; gap:12px; padding:9px 10px; border-radius:8px; font-size:13.5px; }
  .extrow:hover { background:rgba(0,0,0,.05); }
  @media (prefers-color-scheme: dark) { .extrow:hover { background:rgba(255,255,255,.07); } }
  .extrow .nm { flex:1; overflow:hidden; text-overflow:ellipsis; white-space:nowrap; }
  .extrow .nm.dis { opacity:.5; }
  .extrow button.mini { border:none; background:none; color:#888; cursor:pointer; font-size:12.5px;
                        padding:4px 8px; border-radius:6px; }
  .extrow button.mini:hover { background:rgba(0,0,0,.08); color:inherit; }
  .extrow button.mini.rm:hover { color:#e0555f; }
  input[type=checkbox] { accent-color:#4a80f5; width:15px; height:15px; }
  .hint { font-size:12.5px; color:#888; margin-top:8px; line-height:1.5; }
  .extcat { display:grid; grid-template-columns:repeat(auto-fill,minmax(240px,1fr)); gap:10px; margin-bottom:14px; }
  .extcard { display:flex; align-items:center; gap:12px; padding:12px; border-radius:12px;
             background:rgba(0,0,0,.035); }
  @media (prefers-color-scheme: dark) { .extcard { background:rgba(255,255,255,.05); } }
  .extcard .ei { width:38px; height:38px; flex:none; border-radius:10px; display:flex; align-items:center;
                 justify-content:center; font-size:18px; font-weight:700; color:#fff;
                 background:linear-gradient(135deg,#4a80f5,#7c5cff); }
  .extcard .em { flex:1; min-width:0; }
  .extcard .en { font-weight:600; font-size:13.5px; }
  .extcard .ed { font-size:12px; color:#888; margin-top:2px; line-height:1.35; }
  .extcard button { border:none; border-radius:8px; padding:7px 13px; font:12.5px "Segoe UI",system-ui;
                    font-weight:600; cursor:pointer; flex:none; background:#4a80f5; color:#fff; }
  .extcard button:hover:not(:disabled) { background:#3a6fe0; }
  .extcard button:disabled { background:rgba(0,0,0,.08); color:#8a8a8a; cursor:default; }
  @media (prefers-color-scheme: dark) { .extcard button:disabled { background:rgba(255,255,255,.1); color:#999; } }
</style></head><body>
  <h1>Settings</h1>
  <section>
    <h2>Search engine</h2>
    <div id="engines">)HTML"
         << engines.str() << LR"HTML(</div>
  </section>
  <section>
    <h2>Privacy</h2>
    <label class="switch"><span>Block ads &amp; trackers<div class="sub">Blocks requests to known ad and
      analytics networks. The shield in the toolbar shows what was blocked per page.</div></span>
      <input type="checkbox" id="adblock")HTML"
         << (g_settings.adblock ? L" checked" : L"") << LR"HTML(></label>
    <label class="switch"><span>Restore previous session<div class="sub">Reopen the tabs you had open
      the last time Minima closed.</div></span>
      <input type="checkbox" id="restore")HTML"
         << (g_settings.restoreSession ? L" checked" : L"") << LR"HTML(></label>
  </section>
  <section>
    <h2>Extensions</h2>)HTML";
    if (extSupported) {
        // Featured catalog: match installed extensions by name so we can show "Installed".
        std::wstring installedLower;
        for (auto& e : exts) {
            std::wstring n = e.name;
            std::transform(n.begin(), n.end(), n.begin(), towlower);
            installedLower += n + L"\n";
        }
        html << L"<div class='extcat'>";
        for (auto& c : kExtCatalog) {
            std::wstring nameLower = c.name;
            std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(), towlower);
            bool installed = installedLower.find(nameLower) != std::wstring::npos;
            bool busyThis = g_extBusy.load() && g_extBusyName == c.name;
            bool busyOther = g_extBusy.load() && g_extBusyName != c.name;
            const wchar_t* label = installed ? L"Installed" : busyThis ? L"Installing…" : L"Install";
            bool disabled = installed || g_extBusy.load();
            html << L"<div class='extcard'><div class='ei'>" << c.name[0] << L"</div><div class='em'>"
                 << L"<div class='en'>" << c.name << L"</div><div class='ed'>" << c.desc << L"</div></div>"
                 << L"<button data-cid='" << c.id << L"'" << (disabled ? L" disabled" : L"") << L">"
                 << label << L"</button></div>";
            (void)busyOther;
        }
        html << L"</div>";
    }
    html << LR"HTML(
    <div id="exts">)HTML";
    if (!extSupported) {
        html << L"<p class='hint'>Extensions are not supported by the installed WebView2 runtime.</p>";
    } else if (exts.empty()) {
        html << L"<p class='hint' style='padding:0 10px'>No extensions installed yet.</p>";
    } else {
        for (auto& e : exts) {
            html << L"<div class='extrow'><input type='checkbox' data-id='" << JsonEscape(e.id) << L"'"
                 << (e.enabled ? L" checked" : L"") << L" title='Enable or disable'>"
                 << L"<span class='nm" << (e.enabled ? L"" : L" dis") << L"'>" << JsonEscape(e.name)
                 << L"</span><button class='mini rm' data-id='" << JsonEscape(e.id)
                 << L"'>Remove</button></div>";
        }
    }
    html << LR"HTML(</div>
    <div class="actions" style="margin-top:10px">
      <button class="btn" id="extAdd")HTML" << (extSupported ? L"" : L" disabled") << LR"HTML(>
        Install unpacked extension&hellip;</button>
    </div>
    <p class="hint">Pick a folder containing an unpacked Chrome extension (it must contain
      <b>manifest.json</b>) &mdash; for example uBlock Origin from its GitHub releases. Newly installed
      extensions take effect in tabs opened afterwards.</p>
  </section>
  <section>
    <h2>On-device AI model</h2>
    <div id="models">)HTML"
         << models.str() << LR"HTML(</div>
    <p id="note"></p>
  </section>
  <section>
    <h2>Default browser</h2>
    <div class="actions">
      <button class="btn" id="setDefault">Set Minima as default browser&hellip;</button>
    </div>
    <p class="hint">)HTML"
         << (IsBrowserRegistered()
                 ? L"Minima is registered with Windows. Clicking opens <b>Default apps</b>, where you "
                   L"can pick Minima for links and web files."
                 : L"Registers Minima with Windows (no admin needed) and opens <b>Default apps</b>, "
                   L"where you can pick Minima for links and web files.")
         << LR"HTML(</p>
  </section>
  <section>
    <h2>Data</h2>
    <div class="actions">
      <button class="btn danger" id="clearHistory">Clear browsing history</button>
      <button class="btn danger" id="clearBookmarks">Clear bookmarks</button>
    </div>
  </section>
<script>
  const send = (cmd, arg='') => window.chrome.webview.postMessage(cmd + '\x1F' + arg);
  const note = document.getElementById('note');
  document.getElementById('engines').addEventListener('change', (e) => {
    if (e.target.name === 'se') send('set-search', e.target.value);
  });
  document.getElementById('models').addEventListener('change', (e) => {
    if (e.target.name === 'am') {
      send('set-model', e.target.value);
      note.textContent = 'Switched. It will finish downloading automatically the next time you use AI.';
    }
  });
  document.getElementById('adblock').addEventListener('change', (e) => {
    send('set-adblock', e.target.checked ? '1' : '0');
  });
  document.getElementById('restore').addEventListener('change', (e) => {
    send('set-restore', e.target.checked ? '1' : '0');
  });
  document.getElementById('setDefault').onclick = () => send('set-default');
  document.getElementById('extAdd').onclick = () => send('ext-add');
  document.querySelectorAll('.extcard button').forEach((b) => {
    b.onclick = () => {
      if (b.disabled) return;
      document.querySelectorAll('.extcard button').forEach((o) => { o.disabled = true; });
      b.textContent = 'Installing…';
      send('ext-install', b.dataset.cid);
    };
  });
  document.getElementById('exts').addEventListener('change', (e) => {
    if (e.target.dataset.id) send('ext-toggle', e.target.dataset.id);
  });
  document.getElementById('exts').addEventListener('click', (e) => {
    const b = e.target.closest('button.rm');
    if (b && confirm('Remove this extension?')) send('ext-remove', b.dataset.id);
  });
  document.getElementById('clearHistory').onclick = () => {
    if (confirm('Clear all browsing history? This cannot be undone.')) send('clear-history');
  };
  document.getElementById('clearBookmarks').onclick = () => {
    if (confirm('Remove all bookmarks? This cannot be undone.')) send('clear-bookmarks');
  };
</script></body></html>)HTML";
    return html.str();
}

/// Renders `html` into every open settings tab, or opens a new one (unless refreshOnly).
static void ShowSettingsHtml(const std::wstring& html, bool refreshOnly) {
    bool found = false;
    for (auto& t : g_app.tabs) {
        if (t->isSettingsPage && t->webview) {
            t->webview->NavigateToString(html.c_str());
            found = true;
        }
    }
    if (found) {
        if (!refreshOnly) {
            for (auto& t : g_app.tabs) {
                if (t->isSettingsPage) { g_app.activeId = t->id; break; }
            }
            Relayout();
            SyncState();
        }
        return;
    }
    if (!refreshOnly) CreateTab(L"", html, PageKind::Settings);
}

/// Opens (or refreshes) the settings page; queries installed extensions first.
static void OpenSettings(bool refreshOnly) {
    ComPtr<ICoreWebView2Profile7> p7;
    if (g_app.profile) g_app.profile.As(&p7);
    if (!p7) {
        ShowSettingsHtml(BuildSettingsHtml({}, false), refreshOnly);
        return;
    }
    p7->GetBrowserExtensions(
        Callback<ICoreWebView2ProfileGetBrowserExtensionsCompletedHandler>(
            [refreshOnly](HRESULT hr, ICoreWebView2BrowserExtensionList* list) -> HRESULT {
                std::vector<ExtInfo> exts;
                if (SUCCEEDED(hr) && list) {
                    UINT32 n = 0;
                    list->get_Count(&n);
                    for (UINT32 i = 0; i < n; i++) {
                        ComPtr<ICoreWebView2BrowserExtension> e;
                        if (FAILED(list->GetValueAtIndex(i, &e)) || !e) continue;
                        ExtInfo info;
                        CoStr id, name;
                        BOOL en = TRUE;
                        if (SUCCEEDED(e->get_Id(&id)) && id) info.id = id.get();
                        if (SUCCEEDED(e->get_Name(&name)) && name) info.name = name.get();
                        e->get_IsEnabled(&en);
                        info.enabled = en;
                        exts.push_back(std::move(info));
                    }
                }
                ShowSettingsHtml(BuildSettingsHtml(exts, true), refreshOnly);
                return S_OK;
            })
            .Get());
}

/// Modal folder picker (for unpacked extensions).
static std::wstring PickFolder(const wchar_t* title) {
    std::wstring result;
    ComPtr<IFileOpenDialog> dlg;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&dlg)))) {
        DWORD opts = 0;
        dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_PICKFOLDERS);
        dlg->SetTitle(title);
        if (SUCCEEDED(dlg->Show(g_app.hwnd))) {
            ComPtr<IShellItem> item;
            if (SUCCEEDED(dlg->GetResult(&item)) && item) {
                CoStr path;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path)
                    result = path.get();
            }
        }
    }
    return result;
}

/// Runs `op(extension)` on the installed extension with the given id.
template <typename Fn>
static void WithExtension(const std::wstring& extId, Fn op) {
    ComPtr<ICoreWebView2Profile7> p7;
    if (!g_app.profile || FAILED(g_app.profile.As(&p7)) || !p7) return;
    p7->GetBrowserExtensions(
        Callback<ICoreWebView2ProfileGetBrowserExtensionsCompletedHandler>(
            [extId, op](HRESULT hr, ICoreWebView2BrowserExtensionList* list) -> HRESULT {
                if (FAILED(hr) || !list) return S_OK;
                UINT32 n = 0;
                list->get_Count(&n);
                for (UINT32 i = 0; i < n; i++) {
                    ComPtr<ICoreWebView2BrowserExtension> e;
                    if (FAILED(list->GetValueAtIndex(i, &e)) || !e) continue;
                    CoStr id;
                    if (SUCCEEDED(e->get_Id(&id)) && id && extId == id.get()) {
                        op(e.Get());
                        break;
                    }
                }
                return S_OK;
            })
            .Get());
}

static void InstallExtensionFromFolder() {
    std::wstring path = PickFolder(L"Choose an unpacked extension folder (must contain manifest.json)");
    if (path.empty()) return;
    ComPtr<ICoreWebView2Profile7> p7;
    if (!g_app.profile || FAILED(g_app.profile.As(&p7)) || !p7) return;
    p7->AddBrowserExtension(
        path.c_str(),
        Callback<ICoreWebView2ProfileAddBrowserExtensionCompletedHandler>(
            [](HRESULT hr, ICoreWebView2BrowserExtension*) -> HRESULT {
                if (FAILED(hr)) {
                    MessageBoxW(g_app.hwnd,
                                L"Could not install the extension. Make sure the folder contains a valid "
                                L"manifest.json (an unpacked Chrome extension).",
                                L"Minima", MB_ICONWARNING);
                } else {
                    MessageBoxW(g_app.hwnd,
                                L"Extension installed. It applies to tabs opened from now on.",
                                L"Minima", MB_ICONINFORMATION);
                }
                OpenSettings(true);
                return S_OK;
            })
            .Get());
}

/// Resolves the browser_download_url of the first release asset whose name contains `match`.
static std::wstring FindGithubAssetUrl(const std::wstring& repoPath, const std::string& match) {
    std::string json = HttpGetText(L"api.github.com", repoPath, nullptr);
    return Utf8ToWide(ParseGithubAssetUrl(json, match));
}

struct ExtWork { std::wstring id, name, repoPath; std::string match; };

/// Worker thread: resolve → download → extract → locate manifest, then hand the folder
/// back to the UI thread (AddBrowserExtension is a profile/COM call and must run there).
static DWORD WINAPI ExtInstallThread(LPVOID p) {
    std::unique_ptr<ExtWork> w(static_cast<ExtWork*>(p));
    auto post = [](bool ok, const std::wstring& payload) {
        PostMessageW(g_app.hwnd, WM_APP_EXT_DONE, 0,
                     reinterpret_cast<LPARAM>(new std::pair<bool, std::wstring>(ok, payload)));
    };
    std::wstring url = FindGithubAssetUrl(w->repoPath, w->match);
    if (url.empty()) { post(false, L"Could not find a download for " + w->name + L"."); return 0; }
    std::wstring cache = g_dataDir + L"\\extcache";
    CreateDirectoryW(cache.c_str(), nullptr);
    std::wstring zipPath = cache + L"\\" + w->id + L".zip";
    if (!DownloadFile(url, zipPath, nullptr, nullptr)) {
        post(false, L"Download failed for " + w->name + L".");
        return 0;
    }
    std::wstring dest = cache + L"\\" + w->id;
    RunAndWait(L"cmd /c rmdir /s /q \"" + dest + L"\"", 15000); // clear any previous copy
    CreateDirectoryW(dest.c_str(), nullptr);
    RunAndWait(L"tar.exe -xf \"" + zipPath + L"\" -C \"" + dest + L"\"", 120000);
    DeleteFileW(zipPath.c_str());
    std::wstring manDir = FindManifestDir(dest);
    if (manDir.empty()) { post(false, L"The downloaded package for " + w->name + L" was not a valid extension."); return 0; }
    post(true, manDir);
    return 0;
}

/// Kicks off a background install of a catalog extension by id.
static void InstallCatalogExtension(const std::wstring& id) {
    const CatalogExt* found = nullptr;
    for (auto& c : kExtCatalog)
        if (id == c.id) { found = &c; break; }
    if (!found) return;
    if (g_extBusy.exchange(true)) return; // one install at a time
    g_extBusyName = found->name;
    auto* w = new ExtWork{found->id, found->name, found->repoPath, found->assetMatch};
    HANDLE h = CreateThread(nullptr, 0, ExtInstallThread, w, 0, nullptr);
    if (h) {
        CloseHandle(h);
    } else {
        delete w;
        g_extBusy = false;
    }
    OpenSettings(true); // reflect the "Installing…" state
}

/// Dispatches commands posted from the (isSettingsPage-gated) settings tab.
static void HandleSettingsCommand(const std::wstring& msg) {
    std::wstring cmd = msg, arg;
    size_t sep = msg.find(L'\x1F');
    if (sep != std::wstring::npos) {
        cmd = msg.substr(0, sep);
        arg = msg.substr(sep + 1);
    }
    if (cmd == L"set-search") {
        g_settings.searchEngine = arg;
        SaveSettings();
    } else if (cmd == L"set-model") {
        SwitchAiModel(arg);
    } else if (cmd == L"set-adblock") {
        g_settings.adblock = arg != L"0";
        SaveSettings();
        SyncState();
    } else if (cmd == L"set-restore") {
        g_settings.restoreSession = arg != L"0";
        SaveSettings();
    } else if (cmd == L"set-default") {
        RegisterBrowser();
        ShellExecuteW(nullptr, L"open", L"ms-settings:defaultapps", nullptr, nullptr, SW_SHOWNORMAL);
        OpenSettings(true); // refresh the hint to the "registered" wording
    } else if (cmd == L"ext-add") {
        InstallExtensionFromFolder();
    } else if (cmd == L"ext-install") {
        InstallCatalogExtension(arg);
    } else if (cmd == L"ext-toggle") {
        WithExtension(arg, [](ICoreWebView2BrowserExtension* e) {
            BOOL en = TRUE;
            e->get_IsEnabled(&en);
            e->Enable(!en,
                      Callback<ICoreWebView2BrowserExtensionEnableCompletedHandler>(
                          [](HRESULT) -> HRESULT {
                              OpenSettings(true);
                              return S_OK;
                          })
                          .Get());
        });
    } else if (cmd == L"ext-remove") {
        WithExtension(arg, [](ICoreWebView2BrowserExtension* e) {
            e->Remove(Callback<ICoreWebView2BrowserExtensionRemoveCompletedHandler>(
                          [](HRESULT) -> HRESULT {
                              OpenSettings(true);
                              return S_OK;
                          })
                          .Get());
        });
    } else if (cmd == L"clear-history") {
        g_history.clear();
        SaveHistory();
    } else if (cmd == L"clear-bookmarks") {
        g_bookmarks.clear();
        SaveBookmarks();
    }
}

/// Dispatches commands from the first-run wizard page. On "wiz-done" it persists
/// settings (settings.json doubles as the "already configured" marker) and turns
/// the wizard tab into a normal start page.
static void HandleWizardCommand(Tab* t, const std::wstring& msg) {
    std::wstring cmd = msg, arg;
    size_t sep = msg.find(L'\x1F');
    if (sep != std::wstring::npos) {
        cmd = msg.substr(0, sep);
        arg = msg.substr(sep + 1);
    }
    if (cmd == L"wiz-search") {
        g_settings.searchEngine = arg;
    } else if (cmd == L"wiz-adblock") {
        g_settings.adblock = arg != L"0";
    } else if (cmd == L"wiz-ai") {
        StartAiSetup();
    } else if (cmd == L"wiz-done") {
        SaveSettings();
        SyncState();
        if (t && t->webview) {
            t->isWizardPage = false;
            t->title = L"New Tab";
            t->webview->NavigateToString(BuildStartHtml().c_str());
            if (g_app.chromeController && g_app.chromeWebview) {
                g_app.chromeController->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
                g_app.chromeWebview->ExecuteScript(L"document.getElementById('addr').focus()", nullptr);
            }
        }
    }
}

/// Reopens the most recently closed tab (Ctrl+Shift+T).
static void ReopenClosedTab() {
    if (g_app.closedUrls.empty()) return;
    std::wstring url = g_app.closedUrls.back();
    g_app.closedUrls.pop_back();
    CreateTab(url);
}

/// Cycles the active tab by `dir` (+1 = next, -1 = previous), wrapping around.
static void CycleTab(int dir) {
    if (g_app.tabs.size() < 2) return;
    int idx = 0;
    for (size_t i = 0; i < g_app.tabs.size(); i++)
        if (g_app.tabs[i]->id == g_app.activeId) { idx = static_cast<int>(i); break; }
    int n = static_cast<int>(g_app.tabs.size());
    g_app.activeId = g_app.tabs[(idx + dir + n) % n]->id;
    Relayout();
    SyncState();
}

static void SelectTabAt(int num) { // 1-based; 9 = last (like Chrome)
    if (g_app.tabs.empty()) return;
    size_t idx = num >= 9 ? g_app.tabs.size() - 1
                          : std::min(static_cast<size_t>(num - 1), g_app.tabs.size() - 1);
    g_app.activeId = g_app.tabs[idx]->id;
    Relayout();
    SyncState();
}

/// factor > 0 multiplies the active tab's zoom; 0 resets it to 100%.
static void ZoomActive(double factor) {
    Tab* t = ActiveTab();
    if (!t || !t->controller) return;
    t->zoom = factor == 0 ? 1.0 : std::max(0.25, std::min(5.0, t->zoom * factor));
    t->controller->put_ZoomFactor(t->zoom);
}

static void ToggleWindowFullscreen() {
    static WINDOWPLACEMENT prev{sizeof(prev)};
    DWORD style = static_cast<DWORD>(GetWindowLongPtrW(g_app.hwnd, GWL_STYLE));
    if (style & WS_OVERLAPPEDWINDOW) {
        MONITORINFO mi{sizeof(mi)};
        if (GetWindowPlacement(g_app.hwnd, &prev) &&
            GetMonitorInfoW(MonitorFromWindow(g_app.hwnd, MONITOR_DEFAULTTONEAREST), &mi)) {
            SetWindowLongPtrW(g_app.hwnd, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
            SetWindowPos(g_app.hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                         mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                         SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        }
    } else {
        SetWindowLongPtrW(g_app.hwnd, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(g_app.hwnd, &prev);
        SetWindowPos(g_app.hwnd, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
}

/// Opens the Ctrl+K command palette (rendered inside the chrome webview).
static void OpenCommandPalette() {
    if (g_app.chromeController && g_app.chromeWebview) {
        g_app.chromeController->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
        g_app.chromeWebview->ExecuteScript(L"window.__openPalette && window.__openPalette()", nullptr);
    }
}

/// Browser-level shortcuts that must work even while a page has focus.
static bool HandleAccelerator(UINT key, bool ctrl, bool alt, bool shift) {
    Tab* t = ActiveTab();
    if (ctrl) {
        if (key >= '1' && key <= '9') { SelectTabAt(static_cast<int>(key - '0')); return true; }
        switch (key) {
            case 'T':
                if (shift) ReopenClosedTab(); else CreateTab(L"");
                return true;
            case 'W': if (t) CloseTab(t->id); return true;
            case 'R': if (t && t->webview) t->webview->Reload(); return true;
            case 'D':
                if (t) { ToggleBookmark(t->url, t->title); SyncState(); }
                return true;
            case 'H': CreateTab(L"", BuildHistoryHtml()); return true;
            case 'K': OpenCommandPalette(); return true;
            case 'F': OpenFindBar(); return true;
            case 'J':
                if (t && t->webview) {
                    ComPtr<ICoreWebView2_9> wv9;
                    if (SUCCEEDED(t->webview.As(&wv9)) && wv9) wv9->OpenDefaultDownloadDialog();
                }
                return true;
            case VK_TAB: CycleTab(shift ? -1 : 1); return true;
            case VK_OEM_COMMA: OpenSettings(false); return true;
            case VK_OEM_PLUS: case VK_ADD: ZoomActive(1.1); return true;
            case VK_OEM_MINUS: case VK_SUBTRACT: ZoomActive(1.0 / 1.1); return true;
            case '0': case VK_NUMPAD0: ZoomActive(0); return true;
            case 'L':
                if (g_app.chromeController) {
                    g_app.chromeController->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
                    if (g_app.chromeWebview)
                        g_app.chromeWebview->ExecuteScript(L"document.getElementById('addr').focus()", nullptr);
                }
                return true;
        }
    }
    if (alt) {
        if (key == VK_LEFT && t && t->webview) { t->webview->GoBack(); return true; }
        if (key == VK_RIGHT && t && t->webview) { t->webview->GoForward(); return true; }
    }
    if (key == VK_F5 && t && t->webview) { t->webview->Reload(); return true; }
    if (key == VK_F11) { ToggleWindowFullscreen(); return true; }
    return false;
}

static void WireTabEvents(Tab* tab) {
    ICoreWebView2* wv = tab->webview.Get();
    std::wstring id = tab->id;

    EventRegistrationToken tok;
    wv->add_DocumentTitleChanged(
        Callback<ICoreWebView2DocumentTitleChangedEventHandler>(
            [id](ICoreWebView2* sender, IUnknown*) -> HRESULT {
                if (Tab* t = FindTab(id)) {
                    CoStr title;
                    if (SUCCEEDED(sender->get_DocumentTitle(&title)) && title)
                        t->title = title.get();
                    SyncState();
                }
                return S_OK;
            })
            .Get(),
        &tok);

    wv->add_SourceChanged(
        Callback<ICoreWebView2SourceChangedEventHandler>(
            [id](ICoreWebView2* sender, ICoreWebView2SourceChangedEventArgs*) -> HRESULT {
                if (Tab* t = FindTab(id)) {
                    CoStr uri;
                    if (SUCCEEDED(sender->get_Source(&uri)) && uri) t->url = uri.get();
                    SyncState();
                }
                return S_OK;
            })
            .Get(),
        &tok);

    wv->add_NavigationStarting(
        Callback<ICoreWebView2NavigationStartingEventHandler>(
            [id](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs*) -> HRESULT {
                if (Tab* t = FindTab(id)) {
                    t->loading = true;
                    t->blocked = 0;
                    t->favicon.clear();
                    SyncState();
                }
                return S_OK;
            })
            .Get(),
        &tok);

    // Network-level ad/tracker blocking for every request this page makes.
    wv->AddWebResourceRequestedFilter(L"*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
    wv->add_WebResourceRequested(
        Callback<ICoreWebView2WebResourceRequestedEventHandler>(
            [id](ICoreWebView2*, ICoreWebView2WebResourceRequestedEventArgs* args) -> HRESULT {
                if (!g_settings.adblock || !args) return S_OK;
                ComPtr<ICoreWebView2WebResourceRequest> req;
                if (FAILED(args->get_Request(&req)) || !req) return S_OK;
                CoStr uri;
                if (FAILED(req->get_Uri(&uri)) || !uri) return S_OK;
                if (!IsAdUrl(uri.get())) return S_OK;
                ComPtr<ICoreWebView2WebResourceResponse> resp;
                if (g_app.env &&
                    SUCCEEDED(g_app.env->CreateWebResourceResponse(nullptr, 403, L"Blocked", L"", &resp)))
                    args->put_Response(resp.Get());
                if (Tab* t = FindTab(id)) {
                    t->blocked++;
                    ScheduleSync();
                }
                return S_OK;
            })
            .Get(),
        &tok);

    // Favicon for the tab strip.
    ComPtr<ICoreWebView2_15> wv15;
    if (SUCCEEDED(wv->QueryInterface(IID_PPV_ARGS(&wv15)))) {
        wv15->add_FaviconChanged(
            Callback<ICoreWebView2FaviconChangedEventHandler>(
                [id](ICoreWebView2* sender, IUnknown*) -> HRESULT {
                    ComPtr<ICoreWebView2_15> s15;
                    if (SUCCEEDED(sender->QueryInterface(IID_PPV_ARGS(&s15)))) {
                        CoStr uri;
                        if (SUCCEEDED(s15->get_FaviconUri(&uri)) && uri) {
                            if (Tab* t = FindTab(id)) {
                                t->favicon = uri.get();
                                ScheduleSync();
                            }
                        }
                    }
                    return S_OK;
                })
                .Get(),
            &tok);
    }

    // Audio playing/muted indicators.
    ComPtr<ICoreWebView2_8> wv8;
    if (SUCCEEDED(wv->QueryInterface(IID_PPV_ARGS(&wv8)))) {
        auto onAudio = Callback<ICoreWebView2IsDocumentPlayingAudioChangedEventHandler>(
            [id](ICoreWebView2* sender, IUnknown*) -> HRESULT {
                ComPtr<ICoreWebView2_8> s8;
                if (SUCCEEDED(sender->QueryInterface(IID_PPV_ARGS(&s8)))) {
                    if (Tab* t = FindTab(id)) {
                        BOOL b = FALSE;
                        s8->get_IsDocumentPlayingAudio(&b);
                        t->audio = b;
                        s8->get_IsMuted(&b);
                        t->muted = b;
                        ScheduleSync();
                    }
                }
                return S_OK;
            });
        wv8->add_IsDocumentPlayingAudioChanged(onAudio.Get(), &tok);
        wv8->add_IsMutedChanged(
            Callback<ICoreWebView2IsMutedChangedEventHandler>(
                [id](ICoreWebView2* sender, IUnknown*) -> HRESULT {
                    ComPtr<ICoreWebView2_8> s8;
                    if (SUCCEEDED(sender->QueryInterface(IID_PPV_ARGS(&s8)))) {
                        if (Tab* t = FindTab(id)) {
                            BOOL b = FALSE;
                            s8->get_IsMuted(&b);
                            t->muted = b;
                            ScheduleSync();
                        }
                    }
                    return S_OK;
                })
                .Get(),
            &tok);
    }

    // HTML fullscreen (e.g. video players): hide the chrome strip while active.
    wv->add_ContainsFullScreenElementChanged(
        Callback<ICoreWebView2ContainsFullScreenElementChangedEventHandler>(
            [](ICoreWebView2* sender, IUnknown*) -> HRESULT {
                BOOL fs = FALSE;
                sender->get_ContainsFullScreenElement(&fs);
                g_app.htmlFullscreen = fs;
                Relayout();
                return S_OK;
            })
            .Get(),
        &tok);

    wv->add_NavigationCompleted(
        Callback<ICoreWebView2NavigationCompletedEventHandler>(
            [id](ICoreWebView2* sender, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
                if (Tab* t = FindTab(id)) {
                    t->loading = false;
                    BOOL b = FALSE;
                    sender->get_CanGoBack(&b);
                    t->canBack = b;
                    sender->get_CanGoForward(&b);
                    t->canFwd = b;
                    BOOL success = FALSE;
                    if (args) args->get_IsSuccess(&success);
                    if (success) AddHistory(t->url, t->title);
                    SyncState();
                    if (id == g_app.activeId) RefreshSidebar();
                }
                return S_OK;
            })
            .Get(),
        &tok);

    // Messages from page content are only honored for our own internal AI/Settings pages —
    // arbitrary websites can call postMessage too, so this must stay gated on the page flags.
    wv->add_WebMessageReceived(
        Callback<ICoreWebView2WebMessageReceivedEventHandler>(
            [id](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                Tab* t = FindTab(id);
                if (!t || !args) return S_OK;
                CoStr msg;
                if (!SUCCEEDED(args->TryGetWebMessageAsString(&msg)) || !msg) return S_OK;
                if (t->isAiPage && wcscmp(msg.get(), L"ai-start") == 0) {
                    StartAiSetup();
                } else if (t->isWizardPage) {
                    HandleWizardCommand(t, msg.get());
                } else if (t->isSettingsPage) {
                    HandleSettingsCommand(msg.get());
                }
                return S_OK;
            })
            .Get(),
        &tok);

    // "Ask Minima AI" on selected text — opens the on-device AI with the selection.
    ComPtr<ICoreWebView2_11> wv11;
    if (SUCCEEDED(wv->QueryInterface(IID_PPV_ARGS(&wv11)))) {
        wv11->add_ContextMenuRequested(
            Callback<ICoreWebView2ContextMenuRequestedEventHandler>(
                [](ICoreWebView2*, ICoreWebView2ContextMenuRequestedEventArgs* args) -> HRESULT {
                    ComPtr<ICoreWebView2ContextMenuTarget> target;
                    BOOL hasSel = FALSE;
                    if (FAILED(args->get_ContextMenuTarget(&target)) || !target) return S_OK;
                    target->get_HasSelection(&hasSel);
                    if (!hasSel) return S_OK;
                    CoStr sel;
                    if (FAILED(target->get_SelectionText(&sel)) || !sel || !*sel.get()) return S_OK;
                    ComPtr<ICoreWebView2Environment9> env9;
                    if (!g_app.env || FAILED(g_app.env.As(&env9)) || !env9) return S_OK;
                    ComPtr<ICoreWebView2ContextMenuItem> item;
                    if (FAILED(env9->CreateContextMenuItem(L"Ask Minima AI",
                                                           nullptr,
                                                           COREWEBVIEW2_CONTEXT_MENU_ITEM_KIND_COMMAND,
                                                           &item)) ||
                        !item)
                        return S_OK;
                    std::wstring query = sel.get();
                    EventRegistrationToken tok2;
                    item->add_CustomItemSelected(
                        Callback<ICoreWebView2CustomItemSelectedEventHandler>(
                            [query](ICoreWebView2ContextMenuItem*, IUnknown*) -> HRESULT {
                                CreateTab(L"", BuildAiHtml(query), PageKind::Ai);
                                return S_OK;
                            })
                            .Get(),
                        &tok2);
                    ComPtr<ICoreWebView2ContextMenuItemCollection> items;
                    if (SUCCEEDED(args->get_MenuItems(&items)) && items) {
                        UINT32 n = 0;
                        items->get_Count(&n);
                        items->InsertValueAtIndex(0, item.Get());
                    }
                    return S_OK;
                })
                .Get(),
            &tok);
    }

    // Open popups/target=_blank as new tabs instead of external windows.
    wv->add_NewWindowRequested(
        Callback<ICoreWebView2NewWindowRequestedEventHandler>(
            [](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
                CoStr uri;
                if (SUCCEEDED(args->get_Uri(&uri)) && uri) CreateTab(uri.get());
                args->put_Handled(TRUE);
                return S_OK;
            })
            .Get(),
        &tok);

    tab->controller->add_AcceleratorKeyPressed(
        Callback<ICoreWebView2AcceleratorKeyPressedEventHandler>(
            [](ICoreWebView2Controller*, ICoreWebView2AcceleratorKeyPressedEventArgs* args) -> HRESULT {
                COREWEBVIEW2_KEY_EVENT_KIND kind;
                args->get_KeyEventKind(&kind);
                if (kind != COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN &&
                    kind != COREWEBVIEW2_KEY_EVENT_KIND_SYSTEM_KEY_DOWN)
                    return S_OK;
                UINT key = 0;
                args->get_VirtualKey(&key);
                bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
                bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
                bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                if (HandleAccelerator(key, ctrl, alt, shift)) args->put_Handled(TRUE);
                return S_OK;
            })
            .Get(),
        &tok);
}

static void CreateTab(const std::wstring& navigateTo, const std::wstring& htmlContent, PageKind kind) {
    std::wstring id = L"t" + std::to_wstring(g_app.nextTabNum++);
    auto tab = std::make_unique<Tab>();
    tab->id = id;
    tab->isAiPage = kind == PageKind::Ai;
    tab->isSettingsPage = kind == PageKind::Settings;
    tab->isWizardPage = kind == PageKind::Wizard;
    Tab* raw = tab.get();
    g_app.tabs.push_back(std::move(tab));
    g_app.activeId = id;
    if (kind == PageKind::Ai) g_aiTabId = id;
    SyncState();

    g_app.env->CreateCoreWebView2Controller(
        g_app.hwnd,
        Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
            [raw, navigateTo, htmlContent](HRESULT hr, ICoreWebView2Controller* controller) -> HRESULT {
                if (FAILED(hr) || !controller) {
                    wchar_t buf[128];
                    swprintf(buf, 128, L"Failed to create tab webview (0x%08lx)", hr);
                    MessageBoxW(g_app.hwnd, buf, L"Minima", MB_ICONERROR);
                    return hr;
                }
                raw->controller = controller;
                controller->get_CoreWebView2(&raw->webview);
                WireTabEvents(raw);
                Relayout();
                if (!navigateTo.empty()) {
                    raw->webview->Navigate(navigateTo.c_str());
                } else if (!htmlContent.empty()) {
                    raw->webview->NavigateToString(htmlContent.c_str());
                } else {
                    raw->webview->NavigateToString(BuildStartHtml().c_str());
                    // Fresh tab: put the cursor straight into the address bar.
                    if (g_app.chromeController && g_app.chromeWebview) {
                        g_app.chromeController->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
                        g_app.chromeWebview->ExecuteScript(
                            L"document.getElementById('addr').focus()", nullptr);
                    }
                }
                SyncState();
                return S_OK;
            })
            .Get());
}

static void CloseTab(const std::wstring& id) {
    auto it = std::find_if(g_app.tabs.begin(), g_app.tabs.end(),
                           [&](const auto& t) { return t->id == id; });
    if (it == g_app.tabs.end()) return;
    size_t idx = static_cast<size_t>(it - g_app.tabs.begin());
    if (!(*it)->url.empty() && (*it)->url.rfind(L"about:", 0) != 0 && (*it)->url.rfind(L"data:", 0) != 0) {
        g_app.closedUrls.push_back((*it)->url);
        if (g_app.closedUrls.size() > 20) g_app.closedUrls.erase(g_app.closedUrls.begin());
    }
    if ((*it)->controller) (*it)->controller->Close();
    g_app.tabs.erase(it);

    if (g_app.tabs.empty()) {
        CreateTab(L"");
        return;
    }
    if (g_app.activeId == id) {
        g_app.activeId = g_app.tabs[std::min(idx, g_app.tabs.size() - 1)]->id;
    }
    Relayout();
    SyncState();
}

/// URL vs. search heuristic; searches go to the user's chosen search engine.
static void NavigateInput(Tab* tab, std::wstring input) {
    if (!tab || !tab->webview) return;
    std::wstring url = ResolveInputToUrl(input, CurrentSearchEngine().urlPrefix);
    tab->webview->Navigate(url.c_str());
}

/// Bookmark/history matches for the address-bar dropdown, posted as {"q":..., "sugg":[...]}.
static void SendSuggestions(const std::wstring& q) {
    if (!g_app.chromeWebview || q.empty()) return;
    std::wstring lq = q;
    std::transform(lq.begin(), lq.end(), lq.begin(), towlower);
    auto matches = [&](const std::wstring& s) {
        std::wstring ls = s;
        std::transform(ls.begin(), ls.end(), ls.begin(), towlower);
        return ls.find(lq) != std::wstring::npos;
    };
    std::wstringstream ss;
    ss << L"{\"q\":\"" << JsonEscape(q) << L"\",\"sugg\":[";
    std::vector<std::wstring> seen;
    int count = 0;
    bool first = true;
    auto add = [&](const wchar_t* kind, const std::wstring& title, const std::wstring& url) {
        if (count >= 6) return;
        for (auto& s : seen)
            if (s == url) return;
        seen.push_back(url);
        if (!first) ss << L",";
        first = false;
        ss << L"{\"kind\":\"" << kind << L"\",\"title\":\"" << JsonEscape(title) << L"\",\"url\":\""
           << JsonEscape(url) << L"\"}";
        count++;
    };
    for (auto& b : g_bookmarks)
        if (matches(b.title) || matches(b.url)) add(L"b", b.title, b.url);
    for (auto& h : g_history) {
        if (count >= 6) break;
        if (matches(h.title) || matches(h.url)) add(L"h", h.title, h.url);
    }
    ss << L"]}";
    g_app.chromeWebview->PostWebMessageAsJson(ss.str().c_str());
}

// ---------------------------------------------------------------------------
// Ask-this-page AI sidebar wiring.
// ---------------------------------------------------------------------------
/// Pushes AI availability + the active page's title/url into the sidebar.
static void PushSidebarState() {
    if (!g_app.sidebarWebview) return;
    Tab* t = ActiveTab();
    std::wstring title = t ? t->title : L"";
    std::wstring url = (t && t->url.rfind(L"data:", 0) != 0) ? t->url : L"";
    std::wstringstream js;
    js << L"window.__sbState && window.__sbState(" << g_ai.port.load() << L"," << g_ai.phase.load()
       << L",\"" << JsonEscape(title) << L"\",\"" << JsonEscape(url) << L"\")";
    g_app.sidebarWebview->ExecuteScript(js.str().c_str(), nullptr);
}

/// Extracts the active page's visible text and pushes it to the sidebar as context.
static void PushSidebarContext() {
    if (!g_app.sidebarWebview) return;
    Tab* t = ActiveTab();
    if (!t || !t->webview) {
        g_app.sidebarWebview->ExecuteScript(L"window.__sbContext && window.__sbContext(\"\")", nullptr);
        return;
    }
    t->webview->ExecuteScript(
        L"(document.body?document.body.innerText:'').slice(0,8000)",
        Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
            [](HRESULT hr, LPCWSTR resultJson) -> HRESULT {
                // resultJson is already a JSON-encoded string (quoted, escaped) — safe to inline.
                std::wstring json = (SUCCEEDED(hr) && resultJson && *resultJson) ? resultJson : L"\"\"";
                if (g_app.sidebarWebview)
                    g_app.sidebarWebview->ExecuteScript(
                        (L"window.__sbContext && window.__sbContext(" + json + L")").c_str(), nullptr);
                return S_OK;
            })
            .Get());
}

static void RefreshSidebar() {
    if (!g_app.sidebarOpen || !g_app.sidebarWebview) return;
    PushSidebarState();
    PushSidebarContext();
}

static void HandleSidebarCommand(const std::wstring& msg) {
    if (msg == L"sb-ready") {
        PushSidebarState();
        PushSidebarContext();
    } else if (msg == L"sb-context") {
        PushSidebarContext();
    } else if (msg == L"sb-setup") {
        StartAiSetup();
    } else if (msg == L"sb-close") {
        g_app.sidebarOpen = false;
        Relayout();
    }
}

static void CreateSidebarWebview() {
    if (!g_app.env) return;
    g_app.env->CreateCoreWebView2Controller(
        g_app.hwnd,
        Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
            [](HRESULT hr, ICoreWebView2Controller* controller) -> HRESULT {
                if (FAILED(hr) || !controller) {
                    g_app.sidebarOpen = false;
                    return hr;
                }
                g_app.sidebarController = controller;
                controller->get_CoreWebView2(&g_app.sidebarWebview);
                EventRegistrationToken tok;
                g_app.sidebarWebview->add_WebMessageReceived(
                    Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                        [](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                            CoStr m;
                            if (SUCCEEDED(args->TryGetWebMessageAsString(&m)) && m)
                                HandleSidebarCommand(m.get());
                            return S_OK;
                        })
                        .Get(),
                    &tok);
                g_app.sidebarController->add_AcceleratorKeyPressed(
                    Callback<ICoreWebView2AcceleratorKeyPressedEventHandler>(
                        [](ICoreWebView2Controller*, ICoreWebView2AcceleratorKeyPressedEventArgs* args) -> HRESULT {
                            COREWEBVIEW2_KEY_EVENT_KIND kind;
                            args->get_KeyEventKind(&kind);
                            if (kind != COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN) return S_OK;
                            UINT key = 0;
                            args->get_VirtualKey(&key);
                            bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
                            bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                            if (ctrl && key != 'L' && key != 'K' && HandleAccelerator(key, ctrl, false, shift))
                                args->put_Handled(TRUE);
                            return S_OK;
                        })
                        .Get(),
                    &tok);
                g_app.sidebarWebview->NavigateToString(kSidebarHtml);
                Relayout();
                return S_OK;
            })
            .Get());
}

static void ToggleSidebar() {
    if (!g_app.sidebarController) {
        g_app.sidebarOpen = true;
        CreateSidebarWebview(); // navigation's sb-ready handler pushes state/context
        return;
    }
    g_app.sidebarOpen = !g_app.sidebarOpen;
    Relayout();
    if (g_app.sidebarOpen) RefreshSidebar();
}

// ---------------------------------------------------------------------------
// Find-in-page (Ctrl+F): native WebView2 Find API with match counts; falls back
// to plain window.find() on runtimes older than the SDK's Find interface.
// ---------------------------------------------------------------------------
static ComPtr<ICoreWebView2Find> g_find;   // active find session (on the active tab)
static bool g_findSubscribed = false;
static EventRegistrationToken g_findTokIdx{}, g_findTokCount{};

/// Pushes {"find":{"i":active,"n":total}} into the chrome UI's find bar.
static void PushFindCounts() {
    if (!g_app.chromeWebview || !g_find) return;
    INT32 idx = 0, n = 0;
    g_find->get_ActiveMatchIndex(&idx);
    g_find->get_MatchCount(&n);
    std::wstringstream ss;
    ss << L"{\"find\":{\"i\":" << idx << L",\"n\":" << n << L"}}";
    g_app.chromeWebview->PostWebMessageAsJson(ss.str().c_str());
}

static void StopFind() {
    if (!g_find) return;
    if (g_findSubscribed) {
        g_find->remove_ActiveMatchIndexChanged(g_findTokIdx);
        g_find->remove_MatchCountChanged(g_findTokCount);
        g_findSubscribed = false;
    }
    g_find->Stop();
    g_find.Reset();
}

static void StartFind(const std::wstring& term) {
    Tab* t = ActiveTab();
    if (!t || !t->webview) return;
    if (term.empty()) {
        StopFind();
        return;
    }
    ComPtr<ICoreWebView2_28> wv28;
    ComPtr<ICoreWebView2Environment15> env15;
    if (SUCCEEDED(t->webview.As(&wv28)) && wv28 && g_app.env &&
        SUCCEEDED(g_app.env.As(&env15)) && env15) {
        ComPtr<ICoreWebView2FindOptions> opts;
        if (SUCCEEDED(env15->CreateFindOptions(&opts)) && opts) {
            opts->put_FindTerm(term.c_str());
            opts->put_ShouldHighlightAllMatches(TRUE);
            opts->put_IsCaseSensitive(FALSE);
            opts->put_ShouldMatchWord(FALSE);
            ComPtr<ICoreWebView2Find> find;
            if (SUCCEEDED(wv28->get_Find(&find)) && find) {
                if (g_find.Get() != find.Get()) {
                    StopFind(); // different tab's session — drop the old one + its tokens
                    g_find = find;
                    auto push = Callback<ICoreWebView2FindActiveMatchIndexChangedEventHandler>(
                        [](ICoreWebView2Find*, IUnknown*) -> HRESULT {
                            PushFindCounts();
                            return S_OK;
                        });
                    g_find->add_ActiveMatchIndexChanged(push.Get(), &g_findTokIdx);
                    g_find->add_MatchCountChanged(
                        Callback<ICoreWebView2FindMatchCountChangedEventHandler>(
                            [](ICoreWebView2Find*, IUnknown*) -> HRESULT {
                                PushFindCounts();
                                return S_OK;
                            })
                            .Get(),
                        &g_findTokCount);
                    g_findSubscribed = true;
                }
                g_find->Start(opts.Get(),
                              Callback<ICoreWebView2FindStartCompletedHandler>(
                                  [](HRESULT) -> HRESULT {
                                      PushFindCounts();
                                      return S_OK;
                                  })
                                  .Get());
                return;
            }
        }
    }
    // Fallback (runtime lacks the Find API): highlight-free jump, no counts shown.
    std::wstring js = L"window.find(\"" + JsonEscape(term) + L"\", false, false, true)";
    t->webview->ExecuteScript(js.c_str(), nullptr);
    if (g_app.chromeWebview)
        g_app.chromeWebview->PostWebMessageAsJson(L"{\"find\":{\"i\":-1,\"n\":-1}}");
}

/// Opens the chrome UI's find bar (Ctrl+F), which drives StartFind over postMessage.
static void OpenFindBar() {
    if (g_app.chromeController && g_app.chromeWebview) {
        g_app.chromeController->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
        g_app.chromeWebview->ExecuteScript(L"window.__openFind && window.__openFind()", nullptr);
    }
}

static void HandleUiCommand(const std::wstring& msg) {
    std::wstring cmd = msg, arg;
    size_t sep = msg.find(L'\x1F');
    if (sep != std::wstring::npos) {
        cmd = msg.substr(0, sep);
        arg = msg.substr(sep + 1);
    }

    if (cmd == L"ready") {
        if (!g_app.tabs.empty()) {
            SyncState();
        } else if (g_firstRun && g_app.initialUrl.empty()) {
            CreateTab(L"", BuildWizardHtml(), PageKind::Wizard);
        } else if (g_app.initialUrl.empty() && g_settings.restoreSession) {
            // Reopen the previous session's tabs; fall back to a fresh tab if none.
            std::vector<SessionTab> session = ParseSession(ReadWideFile(g_dataDir + L"\\session.json"));
            if (session.empty()) {
                CreateTab(L"");
            } else {
                std::wstring currentId;
                for (auto& st : session) {
                    CreateTab(st.url); // sets g_app.activeId to the new tab's id
                    if (st.current) currentId = g_app.activeId;
                }
                if (!currentId.empty()) g_app.activeId = currentId;
                Relayout();
                SyncState();
            }
        } else {
            CreateTab(g_app.initialUrl);
        }
    } else if (cmd == L"navigate") {
        Tab* t = ActiveTab();
        if (!t) { CreateTab(L""); t = ActiveTab(); }
        NavigateInput(t, arg);
    } else if (cmd == L"newtab") {
        CreateTab(L"");
    } else if (cmd == L"close") {
        CloseTab(arg);
    } else if (cmd == L"select") {
        if (FindTab(arg)) {
            StopFind(); // a find session is per-tab; close it when switching
            if (g_app.chromeWebview)
                g_app.chromeWebview->ExecuteScript(L"window.__closeFind && window.__closeFind()", nullptr);
            g_app.activeId = arg;
            Relayout();
            SyncState();
            RefreshSidebar();
        }
    } else if (cmd == L"sidebar") {
        ToggleSidebar();
    } else if (cmd == L"back") {
        if (Tab* t = ActiveTab(); t && t->webview) t->webview->GoBack();
    } else if (cmd == L"forward") {
        if (Tab* t = ActiveTab(); t && t->webview) t->webview->GoForward();
    } else if (cmd == L"reload") {
        if (Tab* t = ActiveTab(); t && t->webview) t->webview->Reload();
    } else if (cmd == L"bookmark") {
        if (Tab* t = ActiveTab()) {
            ToggleBookmark(t->url, t->title);
            SyncState();
        }
    } else if (cmd == L"history") {
        CreateTab(L"", BuildHistoryHtml());
    } else if (cmd == L"askai") {
        CreateTab(L"", BuildAiHtml(arg), PageKind::Ai);
    } else if (cmd == L"settings") {
        OpenSettings(false);
    } else if (cmd == L"query") {
        SendSuggestions(arg);
    } else if (cmd == L"shield") {
        g_settings.adblock = !g_settings.adblock;
        SaveSettings();
        SyncState();
    } else if (cmd == L"reopen") {
        ReopenClosedTab();
    } else if (cmd == L"mute") {
        if (Tab* t = FindTab(arg); t && t->webview) {
            ComPtr<ICoreWebView2_8> w8;
            if (SUCCEEDED(t->webview.As(&w8))) {
                BOOL m = FALSE;
                w8->get_IsMuted(&m);
                w8->put_IsMuted(!m);
            }
        }
    } else if (cmd == L"suggest") {
        g_app.suggestDip = _wtoi(arg.c_str());
        Relayout();
    } else if (cmd == L"reorder") {
        // arg = "<draggedId>|<dropTargetId>": move the dragged tab to the target's slot.
        size_t bar = arg.find(L'|');
        if (bar != std::wstring::npos) {
            std::wstring fromId = arg.substr(0, bar), toId = arg.substr(bar + 1);
            auto fromIt = std::find_if(g_app.tabs.begin(), g_app.tabs.end(),
                                       [&](const auto& t) { return t->id == fromId; });
            auto toIt = std::find_if(g_app.tabs.begin(), g_app.tabs.end(),
                                     [&](const auto& t) { return t->id == toId; });
            if (fromIt != g_app.tabs.end() && toIt != g_app.tabs.end() && fromIt != toIt) {
                auto moved = std::move(*fromIt);
                size_t toIdx = static_cast<size_t>(toIt - g_app.tabs.begin());
                g_app.tabs.erase(fromIt);
                g_app.tabs.insert(g_app.tabs.begin() + std::min(toIdx, g_app.tabs.size()),
                                  std::move(moved));
                SyncState();
            }
        }
    } else if (cmd == L"find") {
        StartFind(arg);
    } else if (cmd == L"findnext") {
        if (g_find) g_find->FindNext();
    } else if (cmd == L"findprev") {
        if (g_find) g_find->FindPrevious();
    } else if (cmd == L"findstop") {
        StopFind();
    } else if (cmd == L"downloads") {
        if (Tab* t = ActiveTab(); t && t->webview) {
            ComPtr<ICoreWebView2_9> wv9;
            if (SUCCEEDED(t->webview.As(&wv9)) && wv9) wv9->OpenDefaultDownloadDialog();
        }
    }
}

static void CreateChromeWebview() {
    g_app.env->CreateCoreWebView2Controller(
        g_app.hwnd,
        Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
            [](HRESULT hr, ICoreWebView2Controller* controller) -> HRESULT {
                if (FAILED(hr) || !controller) {
                    MessageBoxW(g_app.hwnd, L"Failed to create browser UI (WebView2).", L"Minima", MB_ICONERROR);
                    return hr;
                }
                g_app.chromeController = controller;
                controller->get_CoreWebView2(&g_app.chromeWebview);

                // The shared profile (same for all tabs) — used for extension management.
                ComPtr<ICoreWebView2_13> wv13;
                if (SUCCEEDED(g_app.chromeWebview.As(&wv13)) && wv13)
                    wv13->get_Profile(&g_app.profile);

                EventRegistrationToken tok;
                g_app.chromeWebview->add_WebMessageReceived(
                    Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                        [](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                            CoStr msg;
                            if (SUCCEEDED(args->TryGetWebMessageAsString(&msg)) && msg)
                                HandleUiCommand(msg.get());
                            return S_OK;
                        })
                        .Get(),
                    &tok);

                g_app.chromeController->add_AcceleratorKeyPressed(
                    Callback<ICoreWebView2AcceleratorKeyPressedEventHandler>(
                        [](ICoreWebView2Controller*, ICoreWebView2AcceleratorKeyPressedEventArgs* args) -> HRESULT {
                            COREWEBVIEW2_KEY_EVENT_KIND kind;
                            args->get_KeyEventKind(&kind);
                            if (kind != COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN) return S_OK;
                            UINT key = 0;
                            args->get_VirtualKey(&key);
                            bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
                            bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                            // Ctrl+L/K/F/J are handled inside the chrome page itself.
                            if ((ctrl || key == VK_F11) && key != 'L' && key != 'K' && key != 'F' &&
                                key != 'J' && HandleAccelerator(key, ctrl, false, shift))
                                args->put_Handled(TRUE);
                            return S_OK;
                        })
                        .Get(),
                    &tok);

                g_app.chromeWebview->NavigateToString(kChromeHtml);
                Relayout();
                return S_OK;
            })
            .Get());
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_SIZE:
            Relayout();
            return 0;
        case WM_SETFOCUS:
            // Hand keyboard focus to the active page (or the chrome UI).
            if (Tab* t = ActiveTab(); t && t->controller) {
                t->controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
            } else if (g_app.chromeController) {
                g_app.chromeController->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
            }
            return 0;
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN: {
            bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
            bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            if (HandleAccelerator(static_cast<UINT>(wp), ctrl, alt, shift)) return 0;
            break;
        }
        case WM_DPICHANGED: {
            RECT* rc = reinterpret_cast<RECT*>(lp);
            SetWindowPos(hwnd, nullptr, rc->left, rc->top, rc->right - rc->left,
                         rc->bottom - rc->top, SWP_NOZORDER | SWP_NOACTIVATE);
            Relayout();
            return 0;
        }
        case WM_TIMER:
            if (wp == kAiTimerId) {
                PushAiStatus();
                if (g_app.sidebarOpen) PushSidebarState();
                int p = g_ai.phase.load();
                if (p == 0 || p == 5 || p == 6) KillTimer(hwnd, kAiTimerId);
            } else if (wp == kSyncTimerId) {
                KillTimer(hwnd, kSyncTimerId);
                SyncState();
            } else if (wp == kSessionTimerId) {
                KillTimer(hwnd, kSessionTimerId);
                SaveSession();
            }
            return 0;
        case WM_APP_EXT_DONE: {
            std::unique_ptr<std::pair<bool, std::wstring>> res(
                reinterpret_cast<std::pair<bool, std::wstring>*>(lp));
            if (res->first) {
                ComPtr<ICoreWebView2Profile7> p7;
                if (g_app.profile && SUCCEEDED(g_app.profile.As(&p7)) && p7) {
                    p7->AddBrowserExtension(
                        res->second.c_str(),
                        Callback<ICoreWebView2ProfileAddBrowserExtensionCompletedHandler>(
                            [](HRESULT hr, ICoreWebView2BrowserExtension*) -> HRESULT {
                                g_extBusy = false;
                                g_extBusyName.clear();
                                if (FAILED(hr))
                                    MessageBoxW(g_app.hwnd,
                                                L"The extension downloaded but could not be loaded by the "
                                                L"WebView2 runtime.",
                                                L"Minima", MB_ICONWARNING);
                                OpenSettings(true);
                                return S_OK;
                            })
                            .Get());
                } else {
                    g_extBusy = false;
                    g_extBusyName.clear();
                    OpenSettings(true);
                }
            } else {
                g_extBusy = false;
                g_extBusyName.clear();
                MessageBoxW(g_app.hwnd, res->second.c_str(), L"Minima", MB_ICONWARNING);
                OpenSettings(true);
            }
            return 0;
        }
        case WM_COPYDATA: {
            // A second Minima instance forwarding a URL (see wWinMain's single-instance path).
            auto* cds = reinterpret_cast<COPYDATASTRUCT*>(lp);
            if (cds && cds->dwData == 1 && cds->lpData && cds->cbData >= sizeof(wchar_t)) {
                std::wstring url(static_cast<const wchar_t*>(cds->lpData));
                if (!url.empty()) {
                    if (g_app.env) CreateTab(url);
                    else g_app.initialUrl = url; // webview env not up yet — open it on 'ready'
                }
            }
            if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
            SetForegroundWindow(hwnd);
            return TRUE;
        }
        case WM_DESTROY:
            SaveSession();
            if (g_ai.serverStarted.load() && g_ai.serverProc.hProcess) {
                TerminateProcess(g_ai.serverProc.hProcess, 0);
                CloseHandle(g_ai.serverProc.hProcess);
                CloseHandle(g_ai.serverProc.hThread);
            }
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR lpCmdLine, int nCmdShow) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // The shell passes "%1" quoted when Minima is the default browser — strip it.
    std::wstring arg = lpCmdLine ? lpCmdLine : L"";
    while (!arg.empty() && (arg.front() == L' ' || arg.front() == L'"')) arg.erase(arg.begin());
    while (!arg.empty() && (arg.back() == L' ' || arg.back() == L'"')) arg.pop_back();
    if (arg == L"--register") { RegisterBrowser(); return 0; }     // silent, for installers
    if (arg == L"--unregister") { UnregisterBrowser(); return 0; }
    g_app.initialUrl = arg;

    // Single instance: hand the URL (if any) to the running window as a new tab —
    // link clicks from other apps must not spawn a second browser window.
    HANDLE instanceMutex = CreateMutexW(nullptr, TRUE, L"MinimaBrowserSingleInstance");
    if (instanceMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND existing = FindWindowW(L"MinimaBrowser", nullptr)) {
            if (!g_app.initialUrl.empty()) {
                COPYDATASTRUCT cds{};
                cds.dwData = 1;
                cds.cbData = static_cast<DWORD>((g_app.initialUrl.size() + 1) * sizeof(wchar_t));
                cds.lpData = const_cast<wchar_t*>(g_app.initialUrl.c_str());
                SendMessageW(existing, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&cds));
            }
            if (IsIconic(existing)) ShowWindow(existing, SW_RESTORE);
            SetForegroundWindow(existing);
            return 0;
        }
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    HICON appIcon = LoadIconW(hInst, MAKEINTRESOURCEW(101));

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"MinimaBrowser";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.hIcon = appIcon;
    wc.hIconSm = appIcon;
    RegisterClassExW(&wc);

    g_app.hwnd = CreateWindowExW(0, wc.lpszClassName, L"Minima", WS_OVERLAPPEDWINDOW,
                                 CW_USEDEFAULT, CW_USEDEFAULT, 1200, 800, nullptr, nullptr,
                                 hInst, nullptr);

    // Match the title bar to the system theme (dark mode).
    DWORD lightTheme = 1, sz = sizeof(lightTheme);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &lightTheme, &sz);
    BOOL darkTitle = (lightTheme == 0);
    DwmSetWindowAttribute(g_app.hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkTitle, sizeof(darkTitle));

    ShowWindow(g_app.hwnd, nCmdShow);

    // Profile data lives in %LOCALAPPDATA%\Minima.
    PWSTR localAppData = nullptr;
    SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &localAppData);
    std::wstring userData = std::wstring(localAppData ? localAppData : L".") + L"\\Minima";
    CoTaskMemFree(localAppData);
    CreateDirectoryW(userData.c_str(), nullptr);
    g_dataDir = userData;
    g_firstRun = !FileExists(userData + L"\\settings.json");
    LoadBookmarks();
    LoadHistory();
    LoadSettings();
    // Warm the AI engine in the background if it's already installed, so it's ready
    // by the time the user asks something instead of cold-starting on first use.
    if (FileExists(EnginePath()) && FileExists(ModelPath())) StartAiSetup();

    // Enable classic Chrome extensions (installed unpacked via Settings > Extensions).
    auto envOptions = Microsoft::WRL::Make<CoreWebView2EnvironmentOptions>();
    ComPtr<ICoreWebView2EnvironmentOptions6> opt6;
    if (SUCCEEDED(envOptions.As(&opt6)) && opt6) opt6->put_AreBrowserExtensionsEnabled(TRUE);

    CreateCoreWebView2EnvironmentWithOptions(
        nullptr, userData.c_str(), envOptions.Get(),
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [](HRESULT hr, ICoreWebView2Environment* env) -> HRESULT {
                if (FAILED(hr) || !env) {
                    MessageBoxW(g_app.hwnd,
                                L"WebView2 Runtime not available. Install it from Microsoft and retry.",
                                L"Minima", MB_ICONERROR);
                    PostQuitMessage(1);
                    return hr;
                }
                g_app.env = env;
                CreateChromeWebview();
                return S_OK;
            })
            .Get());

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    CoUninitialize();
    return 0;
}
