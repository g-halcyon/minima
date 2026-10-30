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
#define _CRT_RAND_S // rand_s: OS-backed secure random, for the AI server's per-launch key
#include <stdlib.h>
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
#include <filesystem>
#include "WebView2.h"
#include "WebView2EnvironmentOptions.h"
#include "core/json.h"
#include "core/models.h"
#include "core/adblock.h"
#include "core/storage.h"
#include "core/ai_models.h"
#include "core/catalog.h"
#include "core/urls.h"
#include "core/reader.h"
#include "core/memory.h"
#include "core/importer.h"
#include "core/pages.h"
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
    // Restored tabs start without a webview and load the first time they're shown, so a
    // big session doesn't load every page at startup.
    std::wstring pendingUrl;
    bool creating = false;      // controller creation is in flight
    bool suspended = false;     // put to sleep by TrySuspend (background tab, unused a while)
    int memLevel = -1;          // last MemoryUsageTargetLevel set (-1 = never)
    ULONGLONG lastActive = 0;   // GetTickCount64 when the tab was last the active one
    ULONGLONG extractDue = 0;   // when to read the loaded page's article (0 = nothing pending)
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

static void MaterializeTab(Tab* tab, const std::wstring& navigateTo, const std::wstring& htmlContent);
static void ApplyTabSettings(struct Tab* t);

/// Hidden tabs ask WebView2 to trim memory; the visible one runs at normal priority.
static void SetTabMemoryLevel(Tab* t, bool low) {
    int level = low ? COREWEBVIEW2_MEMORY_USAGE_TARGET_LEVEL_LOW : COREWEBVIEW2_MEMORY_USAGE_TARGET_LEVEL_NORMAL;
    if (!t->webview || t->memLevel == level) return;
    ComPtr<ICoreWebView2_19> wv19;
    if (SUCCEEDED(t->webview.As(&wv19)) && wv19 &&
        SUCCEEDED(wv19->put_MemoryUsageTargetLevel(static_cast<COREWEBVIEW2_MEMORY_USAGE_TARGET_LEVEL>(level))))
        t->memLevel = level;
}

static void Relayout() {
    if (!g_app.hwnd) return;
    // Whatever tab just became active must be awake: lazily-restored tabs get their webview
    // now, and tabs suspended in the background are resumed.
    if (Tab* a = ActiveTab()) {
        a->lastActive = GetTickCount64();
        if (!a->controller && !a->creating && !a->pendingUrl.empty()) {
            std::wstring url = a->pendingUrl;
            a->pendingUrl.clear();
            MaterializeTab(a, url, L"");
        } else if (a->suspended && a->webview) {
            ComPtr<ICoreWebView2_3> wv3;
            if (SUCCEEDED(a->webview.As(&wv3)) && wv3) wv3->Resume();
            a->suspended = false;
        }
    }
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
            SetTabMemoryLevel(t.get(), false);
        } else {
            t->controller->put_IsVisible(FALSE);
            SetTabMemoryLevel(t.get(), true);
        }
    }
}

static bool IsBookmarked(const std::wstring& url);
static bool IsAdblockOn();
static bool IsAiEnabled();
static bool IsMemoryOn();

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
    // Per-launch secret the local server requires (--api-key). Only Minima's own pages are
    // given it, so other websites can't drive the model even if they find the port.
    // Not used when the installed engine is too old to accept it from a page (keyActive false).
    // Generated on the UI thread before each setup run; the setup thread only flips keyActive.
    std::wstring key;
    std::atomic<bool> keyActive{false};
    std::atomic<bool> gpuActive{false}; // the running engine offloads to the GPU
    ULONGLONG lastUse = 0; // GetTickCount64 of the last AI request, for idle unloading
};
static AiState g_ai;
static bool AiInstalled();
static std::wstring AiKeyForPages();

/// Push the full tab/address state to the chrome UI.
static void ScheduleSaveSession();

static void SyncState() {
    ScheduleSaveSession(); // tab set/active/url changed — snapshot (debounced)
    if (!g_app.chromeWebview) return;
    std::wstringstream ss;
    ss << L"{\"active\":\"" << JsonEscape(g_app.activeId) << L"\",\"aiPort\":" << g_ai.port.load()
       << L",\"aiPhase\":" << g_ai.phase.load() << L",\"aiKey\":\"" << JsonEscape(AiKeyForPages())
       << L"\",\"aiEnabled\":" << (IsAiEnabled() ? L"true" : L"false")
       << L",\"aiInstalled\":" << (AiInstalled() ? L"true" : L"false")
       << L",\"memory\":" << (IsMemoryOn() ? L"true" : L"false")
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
           << L",\"sleep\":" << ((!t->controller || t->suspended) ? L"true" : L"false")
           << L",\"reader\":" << (t->readerable ? L"true" : L"false")
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

static bool g_historyDirty = false;
static const UINT_PTR kHistoryTimerId = 5;

static void SaveHistory() {
    WriteWideFile(g_dataDir + L"\\history.json", SerializeHistory(g_history));
    g_historyDirty = false;
}

static void ScheduleSaveHistory() {
    g_historyDirty = true;
    if (g_app.hwnd) SetTimer(g_app.hwnd, kHistoryTimerId, 3000, nullptr);
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
    ScheduleSaveHistory(); // the whole file is rewritten, so coalesce bursts of navigations
}

// ---------------------------------------------------------------------------
// Settings: search engine + AI model choice, persisted as a flat JSON object.
// ---------------------------------------------------------------------------

static Settings g_settings;

static bool IsAdblockOn() { return g_settings.adblock; }
static bool IsAiEnabled() { return g_settings.aiEnabled; }
static bool IsMemoryOn() { return g_settings.memory; }

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
        tabs.push_back({t->url, t->id == g_app.activeId, t->title});
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

// ---------------------------------------------------------------------------
// Memory — a private full-text index of pages you've read (core/memory.h), stored in
// memory.db next to history. Off until the user opts in (wizard / start page / Settings).
// ---------------------------------------------------------------------------
static MemoryStore g_memory;
static const long long kMemoryMaxPages = 6000; // with <=50K chars each: bounded, typically tens of MB

/// Opened on first use rather than at startup, to keep launch instant.
static bool MemoryOpen() {
    return g_memory.IsOpen() || g_memory.Open(WideToUtf8(g_dataDir + L"\\memory.db"));
}

static std::wstring HostOf(const std::wstring& url) {
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256]{};
    uc.lpszHostName = host;
    uc.dwHostNameLength = 255;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc) || uc.dwHostNameLength == 0) return L"";
    std::wstring h(host, uc.dwHostNameLength);
    std::transform(h.begin(), h.end(), h.begin(), towlower);
    return h;
}

/// `host` is `domain` or one of its subdomains.
static bool HostIs(const std::wstring& host, const std::wstring& domain) {
    if (domain.empty() || host.size() < domain.size()) return false;
    if (host.compare(host.size() - domain.size(), domain.size(), domain) != 0) return false;
    return host.size() == domain.size() || host[host.size() - domain.size() - 1] == L'.';
}

static std::vector<std::wstring> MemoryExcludedHosts() {
    std::vector<std::wstring> out;
    std::wstringstream ss(g_settings.memoryExclude);
    for (std::wstring line; std::getline(ss, line);)
        if (!line.empty()) out.push_back(line);
    return out;
}

/// Pages Memory never keeps: non-web, sign-in/payment/account flows, and private
/// communication (mail, chat) — plus any site the user excluded.
static bool MemoryEligibleUrl(const std::wstring& url) {
    if (url.rfind(L"https://", 0) != 0 && url.rfind(L"http://", 0) != 0) return false;
    std::wstring lower = url;
    std::transform(lower.begin(), lower.end(), lower.begin(), towlower);
    static const wchar_t* const kSensitivePaths[] = {L"/login", L"/signin", L"/sign-in", L"/logon", L"/auth",
                                                     L"/oauth", L"/checkout", L"/payment", L"/billing",
                                                     L"/account", L"/password", L"/wallet", L"/settings"};
    for (auto p : kSensitivePaths)
        if (lower.find(p) != std::wstring::npos) return false;
    std::wstring host = HostOf(url);
    static const wchar_t* const kPrivateHosts[] = {
        L"mail.google.com", L"outlook.live.com", L"outlook.office.com", L"outlook.office365.com",
        L"mail.yahoo.com", L"mail.proton.me", L"web.whatsapp.com", L"web.telegram.org",
        L"messages.google.com", L"app.slack.com", L"discord.com", L"teams.microsoft.com",
        L"teams.live.com", L"messenger.com", L"chatgpt.com", L"claude.ai"};
    for (auto h : kPrivateHosts)
        if (HostIs(host, h)) return false;
    for (auto& h : MemoryExcludedHosts())
        if (HostIs(host, h)) return false;
    return true;
}

// ---------------------------------------------------------------------------
// The reading pipeline: shortly after each page load, core/reader.h's extractor reads the
// page's article once. That single pass decides whether reader view is offered and feeds
// Memory. (The AI sidebar runs the same extractor live when asked.)
// ---------------------------------------------------------------------------
static const UINT_PTR kExtractTimerId = 6;
static const ULONGLONG kExtractDelayMs = 1500; // let script-built pages render first

static void ScheduleExtract(Tab* t) {
    t->extractDue = GetTickCount64() + kExtractDelayMs;
    if (g_app.hwnd) SetTimer(g_app.hwnd, kExtractTimerId, static_cast<UINT>(kExtractDelayMs + 50), nullptr);
}

static void ExtractTab(Tab* t) {
    if (!t->webview) return;
    std::wstring id = t->id;
    std::wstring js = std::wstring(L"(() => { const R = ") + kExtractJs +
                      L"; return {r: R.readerable ? 1 : 0, w: R.words, pw: R.hasPassword ? 1 : 0, title: R.title,"
                      L" site: R.site, url: R.url, text: R.text.slice(0, 50000)}; })()";
    t->webview->ExecuteScript(
        js.c_str(),
        Callback<ICoreWebView2ExecuteScriptCompletedHandler>([id](HRESULT hr, LPCWSTR json) -> HRESULT {
            Tab* tab = FindTab(id);
            if (!tab || FAILED(hr) || !json) return S_OK;
            std::vector<std::pair<std::wstring, std::wstring>> kv;
            size_t i = 0;
            ParseJsonObject(json, i, kv);
            std::wstring url, title, site, text;
            bool readerable = false, password = false;
            int words = 0;
            for (auto& p : kv) {
                if (p.first == L"url") url = p.second;
                else if (p.first == L"title") title = p.second;
                else if (p.first == L"site") site = p.second;
                else if (p.first == L"text") text = p.second;
                else if (p.first == L"r") readerable = p.second == L"1";
                else if (p.first == L"pw") password = p.second == L"1";
                else if (p.first == L"w") words = _wtoi(p.second.c_str());
            }
            if (url != tab->url) return S_OK; // the tab moved on meanwhile
            if (tab->readerable != readerable) {
                tab->readerable = readerable;
                ScheduleSync();
            }
            if (g_settings.memory && !password && words >= 80 && MemoryEligibleUrl(url) && MemoryOpen()) {
                g_memory.Add(url, title.empty() ? tab->title : title, site, text, static_cast<long long>(time(nullptr)), words);
                static int adds = 0;
                if (++adds % 50 == 0) g_memory.Prune(kMemoryMaxPages);
            }
            return S_OK;
        }).Get());
}

static void RunDueExtractions() {
    ULONGLONG now = GetTickCount64();
    bool pending = false;
    for (auto& t : g_app.tabs) {
        if (!t->extractDue) continue;
        if (now >= t->extractDue) {
            t->extractDue = 0;
            ExtractTab(t.get());
        } else {
            pending = true;
        }
    }
    if (pending && g_app.hwnd) SetTimer(g_app.hwnd, kExtractTimerId, 300, nullptr);
}

static std::wstring ReaderPrefsJson() {
    return L"{\"size\":" + std::to_wstring(g_settings.readerSize) + L",\"font\":\"" + JsonEscape(g_settings.readerFont) +
           L"\",\"theme\":\"" + JsonEscape(g_settings.readerTheme) + L"\"}";
}

/// Opens (or closes) reader view on the active tab (F9 / toolbar book button).
static void ToggleReader() {
    Tab* t = ActiveTab();
    if (!t || !t->webview || (t->url.rfind(L"https://", 0) != 0 && t->url.rfind(L"http://", 0) != 0)) return;
    std::wstring js = std::wstring(L"(") + kReaderJs + L")(" + kExtractJs + L", " + ReaderPrefsJson() + L")";
    t->webview->ExecuteScript(js.c_str(), nullptr);
    if (t->controller) t->controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC); // Esc / scrolling
}

/// Text for HTML content or a quoted attribute — page titles/URLs come from websites
/// (and from imported bookmarks), so they must never be able to inject markup.
static std::wstring HtmlEscape(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size() + 8);
    for (wchar_t c : s) {
        switch (c) {
            case L'&': out += L"&amp;"; break;
            case L'<': out += L"&lt;"; break;
            case L'>': out += L"&gt;"; break;
            case L'"': out += L"&quot;"; break;
            case L'\'': out += L"&#39;"; break;
            default: out += c;
        }
    }
    return out;
}

/// A JSON string body safe inside an inline <script> ("</script>" can't end it early).
static std::wstring JsStr(const std::wstring& s) {
    std::wstring j = JsonEscape(s), out;
    out.reserve(j.size());
    for (wchar_t c : j) {
        if (c == L'<') out += L"\\u003c";
        else if (c == 0x2028 || c == 0x2029) out += c == 0x2028 ? L"\\u2028" : L"\\u2029";
        else out += c;
    }
    return out;
}

static const size_t kStartTiles = 16;

static std::wstring BuildStartHtml() {
    // Bookmarks made in Minima first, then an imported bookmarks bar; capped so a big
    // import doesn't bury the page — everything else is one click away.
    std::vector<const BookmarkEntry*> picks;
    for (int pass = 0; pass < 2 && picks.size() < kStartTiles; pass++)
        for (auto& b : g_bookmarks) {
            if (picks.size() >= kStartTiles) break;
            bool native = b.folder.empty(), bar = b.folder == L"Bookmarks bar";
            if ((pass == 0 && native) || (pass == 1 && bar)) picks.push_back(&b);
        }
    std::wstringstream html;
    html << kStartHtmlHead;
    for (auto* b : picks) {
        std::wstring label = b->title.empty() ? b->url : b->title;
        wchar_t initial = label.empty() ? L'?' : towupper(label[0]);
        html << L"<a class='tile' href=\"" << HtmlEscape(b->url) << L"\" title=\"" << HtmlEscape(b->url) << L"\">"
             << L"<div class='fav'>" << HtmlEscape(std::wstring(1, initial)) << L"</div><span>" << HtmlEscape(label)
             << L"</span></a>";
    }
    html << L"</div>";
    if (g_bookmarks.size() > picks.size())
        html << L"<button class='more' id='allbm'>All bookmarks (" << g_bookmarks.size() << L") &rarr;</button>";
    // Existing users never saw the wizard's Memory step: invite them once, here.
    if (!g_settings.memory && !g_settings.memoryAsked)
        html << L"<div class='card' id='memcard'><b>&#10022; Find anything you've read</b>"
                L"Minima can remember the text of pages you read, so you can search them later &mdash; "
                L"&ldquo;that article about&hellip;&rdquo; from the address bar or Ctrl+H."
                L"<div class='why'>Stored only on this device. Skips sign-in, payment, email and chat pages. "
                L"You can exclude sites or forget everything in Settings.</div>"
                L"<div class='btns'><button class='yes' id='memyes'>Turn on Memory</button>"
                L"<button class='no' id='memno'>Not now</button></div></div>";
    html << L"<script>const send = (c, a='') => window.chrome.webview.postMessage(c + '\\x1F' + a);"
            L"const $ = (id) => document.getElementById(id);"
            L"if ($('allbm')) $('allbm').onclick = () => send('start-bookmarks');"
            L"if ($('memyes')) { $('memyes').onclick = () => { send('start-memory', '1'); $('memcard').remove(); };"
            L" $('memno').onclick = () => { send('start-memory', '0'); $('memcard').remove(); }; }"
            L"</script></body></html>";
    return html.str();
}

static std::wstring BuildHistoryHtml(const std::wstring& query = L"") {
    std::wstringstream html;
    html << kHistoryPageHead << L"const INIT = {q:\"" << JsStr(query) << L"\", memory:"
         << (g_settings.memory ? L"true" : L"false") << L", pages:"
         << ((g_settings.memory && MemoryOpen()) ? g_memory.Count() : 0) << L", recent:[";
    size_t n = std::min<size_t>(g_history.size(), 400);
    for (size_t i = 0; i < n; i++) {
        auto& h = g_history[i];
        html << (i ? L"," : L"") << L"{url:\"" << JsStr(h.url) << L"\",title:\"" << JsStr(h.title) << L"\",time:" << h.time
             << L"}";
    }
    html << L"]};" << kHistoryPageTail;
    return html.str();
}

static std::wstring BuildBookmarksHtml() {
    std::wstringstream html;
    html << kBookmarksPageHead << L"const INIT = {items:[";
    for (size_t i = 0; i < g_bookmarks.size(); i++) {
        auto& b = g_bookmarks[i];
        html << (i ? L"," : L"") << L"{url:\"" << JsStr(b.url) << L"\",title:\"" << JsStr(b.title) << L"\",folder:\""
             << JsStr(b.folder) << L"\"}";
    }
    html << L"]};" << kBookmarksPageTail;
    return html.str();
}

/// The History page's search: Memory full-text hits + title/URL matches, as JSON for
/// window.__results.
static std::wstring HistorySearchJson(const std::wstring& q) {
    std::wstringstream ss;
    ss << L"{\"q\":\"" << JsonEscape(q) << L"\",\"mem\":[";
    if (g_settings.memory && MemoryOpen()) {
        auto hits = g_memory.Search(q, 30);
        for (size_t i = 0; i < hits.size(); i++)
            ss << (i ? L"," : L"") << L"{\"url\":\"" << JsonEscape(hits[i].url) << L"\",\"title\":\""
               << JsonEscape(hits[i].title) << L"\",\"site\":\"" << JsonEscape(hits[i].site) << L"\",\"snippet\":\""
               << JsonEscape(hits[i].snippet) << L"\",\"visited\":" << hits[i].visited << L"}";
    }
    ss << L"],\"hist\":[";
    std::wstring lq = q;
    std::transform(lq.begin(), lq.end(), lq.begin(), towlower);
    int count = 0;
    for (auto& h : g_history) {
        if (count >= 60) break;
        std::wstring hay = h.title + L" " + h.url;
        std::transform(hay.begin(), hay.end(), hay.begin(), towlower);
        if (hay.find(lq) == std::wstring::npos) continue;
        ss << (count++ ? L"," : L"") << L"{\"url\":\"" << JsonEscape(h.url) << L"\",\"title\":\"" << JsonEscape(h.title)
           << L"\",\"time\":" << h.time << L"}";
    }
    ss << L"]}";
    return ss.str();
}

// ---------------------------------------------------------------------------
// First-run setup wizard — a guided onboarding shown the first time Minima
// starts, like any real browser. Renders as an internal page; step navigation
// happens in JS, and each choice is posted to C++ as it's made.
// ---------------------------------------------------------------------------
// Defined with the import/memory machinery further below.
static std::wstring ImportButtonsHtml();
static void StartImport(const std::wstring& id);
static void SetMemoryEnabled(bool on);

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
  .imports { display:flex; flex-wrap:wrap; gap:8px; justify-content:center; }
  .imports .btn { background:rgba(0,0,0,.05); color:inherit; padding:10px 16px; font-weight:600; }
  @media (prefers-color-scheme: dark) { .imports .btn { background:rgba(255,255,255,.08); } }
  .imports .btn:hover:not(:disabled) { background:rgba(74,128,245,.14); }
  .imports .btn:disabled { opacity:.5; cursor:default; }
  .imports .hint { color:#888; font-size:13px; }
  #importNote { font-size:12.5px; color:#4a80f5; text-align:center; margin-top:14px; min-height:16px; }
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
      <h1>Bring your bookmarks</h1>
      <p class="sub">Copy bookmarks (with their folders) and recent history from a browser you already use.</p>
      <div class="imports" id="imports">)HTML"
         << ImportButtonsHtml() << LR"HTML(</div>
      <div id="importNote"></div>
    </div>
    <div class="step" data-step="3">
      <h1>Privacy &amp; memory</h1>
      <p class="sub">Both stay entirely on this device.</p>
      <label class="toggle"><span>Block ads &amp; trackers<div class="d">Recommended &mdash; faster, cleaner, more private browsing.</div></span>
        <span class="sw"><input type="checkbox" id="adblock" checked><span class="tr"></span></span></label>
      <label class="toggle" style="margin-top:10px"><span>Remember pages I read<div class="d">Search the text of anything
        you've read, later. Never keeps sign-in, payment, email or chat pages.</div></span>
        <span class="sw"><input type="checkbox" id="memory" checked><span class="tr"></span></span></label>
    </div>
    <div class="step" data-step="4">
      <h1>On-device AI</h1>
      <p class="sub">Optional. A small Gemma model runs fully on your device.</p>
      <div class="feat"><span class="em">&#129504;</span><div><div class="ft">Set it up now</div><div class="fd">Downloads the engine (~60&nbsp;MB) and model (~800&nbsp;MB) in the background.</div></div></div>
      <label class="toggle"><span>Download AI in the background<div class="d">You can also do this later from the AI page.</div></span>
        <span class="sw"><input type="checkbox" id="aiopt"><span class="tr"></span></span></label>
      <div id="aiNote"></div>
    </div>
    <div class="step" data-step="5">
      <h1>You're all set &#127881;</h1>
      <p class="sub">Minima is ready. Tips: <b>Ctrl+K</b> opens the AI command palette, and the
        sparkle button asks AI about the page you're on.</p>
    </div>
    <div class="nav">
      <button class="ghost" id="back" style="visibility:hidden">Back</button>
      <div class="dots"><i class="on"></i><i></i><i></i><i></i><i></i><i></i></div>
      <button class="primary" id="next">Next</button>
    </div>
  </div>
<script>
  const send = (cmd, arg='') => window.chrome.webview.postMessage(cmd + '\x1F' + arg);
  let step = 0; const last = 5;
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
    if (step === 3) {
      send('wiz-adblock', document.getElementById('adblock').checked ? '1' : '0');
      send('wiz-memory', document.getElementById('memory').checked ? '1' : '0');
    }
    if (step === 4 && document.getElementById('aiopt').checked) send('wiz-ai');
    if (step === last) { send('wiz-done'); return; }
    step++; show();
  };
  document.getElementById('engines').addEventListener('change', (e) => {
    if (e.target.name === 'se') send('wiz-search', e.target.value);
  });
  document.getElementById('imports').addEventListener('click', (e) => {
    const b = e.target.closest('button.imp'); if (!b) return;
    document.querySelectorAll('button.imp').forEach(x => x.disabled = true);
    document.getElementById('importNote').textContent = 'Importing from ' + b.textContent + '…';
    send('wiz-import', b.dataset.src);
  });
  window.__importDone = (id, text, ok) => {
    document.getElementById('importNote').textContent = text;
    document.querySelectorAll('button.imp').forEach(x => { x.disabled = ok && x.dataset.src === id ? true : false; });
  };
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
/// The model file is present *and* full size. Older builds could leave a truncated file
/// behind after an interrupted download; that must not count as installed.
static bool ModelComplete() {
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (!GetFileAttributesExW(ModelPath().c_str(), GetFileExInfoStandard, &fa)) return false;
    long long size = (static_cast<long long>(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow;
    return size >= CurrentAiModel().approxBytes / 100 * 99; // tolerate a re-upload changing it slightly
}
static bool AiInstalled() { return FileExists(EnginePath()) && ModelComplete(); }

/// A Vulkan driver is installed (it ships with GPU drivers). Then Minima uses llama.cpp's
/// Vulkan build in hybrid mode: weights stay in RAM and generation on the CPU, while big
/// prompt batches (reading a page) are offloaded to the GPU — measured on an Iris Xe
/// laptop (Gemma 3 1B): prompt reading 178 -> ~580 tokens/s at the same answer-writing
/// speed (~52/s); end to end, a page summary's first words came 2.5-3.2x sooner (e.g.
/// 9.4 s -> 2.9 s) and the whole summary took 30-49% less time.
static bool VulkanAvailable() {
    wchar_t sys[MAX_PATH];
    UINT n = GetSystemDirectoryW(sys, MAX_PATH);
    return n > 0 && n < MAX_PATH && FileExists(std::wstring(sys) + L"\\vulkan-1.dll");
}
static bool EngineHasVulkan() { return FileExists(BinDir() + L"\\ggml-vulkan.dll"); }

/// True if any of `files` contains the ASCII text `needle` — used to ask an installed
/// engine which command-line flags it understands without launching it.
static bool AnyFileContains(const std::vector<std::wstring>& files, const std::string& needle) {
    for (auto& f : files) {
        HANDLE h = CreateFileW(f.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        LARGE_INTEGER size{};
        std::string data;
        if (GetFileSizeEx(h, &size) && size.QuadPart > 0 && size.QuadPart < (256LL << 20)) {
            data.resize(static_cast<size_t>(size.QuadPart));
            DWORD got = 0;
            if (!ReadFile(h, data.data(), static_cast<DWORD>(data.size()), &got, nullptr)) got = 0;
            data.resize(got);
        }
        CloseHandle(h);
        if (data.find(needle) != std::string::npos) return true;
    }
    return false;
}

/// Extra llama-server flags that lock the local API to Minima's own pages. Engines older than
/// --cors-headers can't let a page send the key (CORS "*" never covers Authorization), so
/// they run without one rather than not at all.
static std::wstring EngineSecurityFlags() {
    // Newer releases keep the argument table in llama-common.dll; older ones link it in.
    std::vector<std::wstring> files = {EnginePath(), BinDir() + L"\\llama-common.dll"};
    if (!AnyFileContains(files, "--cors-headers")) return L"";
    std::wstring flags = L" --api-key " + g_ai.key + L" --cors-headers \"Content-Type,Authorization\"";
    if (AnyFileContains(files, "--no-cors-credentials")) flags += L" --no-cors-credentials";
    g_ai.keyActive = true;
    return flags;
}

/// A fresh random 128-bit key (hex) from the OS CSPRNG.
static std::wstring NewAiKey() {
    wchar_t hex[33];
    for (int i = 0; i < 4; i++) {
        unsigned int r = 0;
        rand_s(&r);
        swprintf(hex + i * 8, 9, L"%08x", r);
    }
    return hex;
}

/// The key as Minima's pages should send it ("" when the engine runs without one).
static std::wstring AiKeyForPages() { return g_ai.keyActive.load() ? g_ai.key : L""; }

/// Download URL of the newest GitHub release asset of `repo` ("owner/name") whose file name
/// contains `match`. With latestOnly, only the release GitHub marks "Latest" is considered;
/// otherwise the newest release that actually has such an asset wins (llama.cpp marks
/// asset-less releases "Latest" while its binaries ship under b<N> tags). Tries the API,
/// then the release web pages (not subject to the API's 60/hour limit). On failure,
/// `rateLimited`/`netErr` say why, for the error message.
#ifndef MINIMA_GITHUB_API_HOST // overridable at build time, e.g. to exercise the fallback in tests
#define MINIMA_GITHUB_API_HOST L"api.github.com"
#endif
static std::wstring ResolveGithubAsset(const std::wstring& repo, const std::string& match, bool latestOnly,
                                       bool* rateLimited = nullptr, DWORD* netErr = nullptr) {
    DWORD err = 0;
    std::string json = HttpGetText(MINIMA_GITHUB_API_HOST,
                                   L"/repos/" + repo + (latestOnly ? L"/releases/latest" : L"/releases?per_page=20"),
                                   &err);
    std::string url = ParseGithubAssetUrl(json, match); // the list is newest-first
    if (rateLimited) *rateLimited = json.find("rate limit") != std::string::npos;
    if (!url.empty()) return Utf8ToWide(url);

    // Latest: that release's own page. Otherwise: the release list, newest first.
    std::vector<std::string> tags = ParseGithubTags(
        HttpGetText(L"github.com", L"/" + repo + (latestOnly ? L"/releases/latest" : L"/releases"), &err));
    size_t tries = latestOnly ? 1 : 6;
    for (size_t i = 0; i < tags.size() && i < tries; i++) {
        std::wstring tag = Utf8ToWide(tags[i]);
        url = ParseGithubDownloadHref(HttpGetText(L"github.com", L"/" + repo + L"/releases/expanded_assets/" + tag, &err),
                                      match);
        if (!url.empty()) return Utf8ToWide(url);
    }
    if (netErr) *netErr = err;
    return L"";
}

static DWORD WINAPI AiSetupThreadProc(LPVOID) {
    CreateDirectoryW(BinDir().c_str(), nullptr);
    CreateDirectoryW(ModelsDir().c_str(), nullptr);

    if (!FileExists(EnginePath())) {
        g_ai.phase = 1;
        g_ai.downloaded = 0;
        g_ai.total = 0;
        DWORD diagErr = 0;
        bool rateLimited = false;
        // llama.cpp's Vulkan build where a Vulkan driver exists (it also contains the CPU
        // backends, so it can always fall back), else the plain CPU build.
        const char* asset = VulkanAvailable() ? "-bin-win-vulkan-x64.zip" : "-bin-win-cpu-x64.zip";
        std::wstring zipUrl = ResolveGithubAsset(L"ggml-org/llama.cpp", asset, false, &rateLimited, &diagErr);
        if (zipUrl.empty()) {
            if (rateLimited) {
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

    if (!ModelComplete()) {
        DeleteFileW(ModelPath().c_str()); // a truncated leftover; a resumable .part is kept
        g_ai.phase = 3;
        g_ai.downloaded = 0;
        g_ai.total = 0;
        if (!DownloadFile(CurrentAiModel().url, ModelPath(), &g_ai.downloaded, &g_ai.total) || !ModelComplete()) {
            g_ai.errorMsg = L"Model download failed. Try again — it will resume where it stopped.";
            g_ai.phase = 6;
            return 0;
        }
    }

    g_ai.phase = 4;
    std::wstring security = EngineSecurityFlags();
    // GPU first when available (hybrid, see VulkanAvailable); if the engine dies while
    // starting — a broken GPU driver — run it again CPU-only rather than failing.
    bool gpu = g_settings.aiGpu && EngineHasVulkan();
    for (int attempt = 0; attempt < 2; attempt++) {
        int port = FindFreePort();
        std::wstring cmd = L"\"" + EnginePath() + L"\" -m \"" + ModelPath() + L"\" --port " + std::to_wstring(port) +
                           L" --host 127.0.0.1 -c 4096 -ngl 0" +
                           // Bigger prompt chunks amortize copying weights to the GPU per chunk
                           // (measured: 490 -> 580 tokens/s reading a page on an Iris Xe).
                           (gpu ? L" -b 2048 -ub 2048" : L" -dev none") + security;
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

        // Wait for real readiness (model loaded, not just the port open) — up to ~2 minutes.
        bool healthy = false, exited = false;
        for (int i = 0; i < 600 && !healthy && !exited; i++) {
            exited = WaitForSingleObject(g_ai.serverProc.hProcess, 200) == WAIT_OBJECT_0;
            if (!exited) healthy = CheckLocalHealth(port);
        }
        if (healthy) {
            g_ai.gpuActive = gpu;
            g_ai.port = port;
            g_ai.phase = 5;
            return 0;
        }
        TerminateProcess(g_ai.serverProc.hProcess, 0);
        CloseHandle(g_ai.serverProc.hProcess);
        CloseHandle(g_ai.serverProc.hThread);
        g_ai.serverProc = PROCESS_INFORMATION{};
        g_ai.serverStarted = false;
        if (!gpu) break;
        gpu = false; // retry on the CPU
    }
    g_ai.errorMsg = L"The AI engine did not start. Deleting the AI files in Settings and setting it up again may help.";
    g_ai.phase = 6;
    return 0;
}

static const UINT_PTR kAiTimerId = 1;

static void StartAiSetup() {
    int p = g_ai.phase.load();
    if (p != 0 && p != 6) return; // already running or ready
    g_ai.phase = 1;
    g_ai.errorMsg.clear();
    g_ai.key = NewAiKey(); // before the thread starts: it only reads the key from here on
    g_ai.keyActive = false;
    g_ai.lastUse = GetTickCount64();
    HANDLE h = CreateThread(nullptr, 0, AiSetupThreadProc, nullptr, 0, nullptr);
    if (h) CloseHandle(h);
    if (g_app.hwnd) SetTimer(g_app.hwnd, kAiTimerId, 400, nullptr);
}

static std::wstring BuildAiHtml(const std::wstring& initialQuery) {
    std::wstringstream html;
    html << kAiHtmlHead;
    html << L"let port = " << g_ai.port.load() << L", aiKey = \"" << JsonEscape(AiKeyForPages())
         << L"\", aiInstalled = " << (AiInstalled() ? L"true" : L"false") << L";\n";
    html << L"let initialQuery = \"" << JsonEscape(initialQuery) << L"\";\n";
    html << L"let initPhase = " << g_ai.phase.load() << L", initDl = " << g_ai.downloaded.load()
         << L", initTotal = " << g_ai.total.load() << L";\n";
    html << kAiHtmlTail;
    return html.str();
}

/// Pushes the current setup phase/progress (and the API key) into every open AI tab.
static void PushAiStatus() {
    SyncState(); // keep the chrome UI's knowledge of aiPort current too
    std::wstringstream js;
    js << L"window.__minimaAiStatus && window.__minimaAiStatus(" << g_ai.phase.load() << L","
       << g_ai.downloaded.load() << L"," << g_ai.total.load() << L"," << g_ai.port.load() << L",\""
       << JsonEscape(g_ai.errorMsg) << L"\",\"" << JsonEscape(AiKeyForPages()) << L"\")";
    for (auto& t : g_app.tabs)
        if (t->isAiPage && t->webview) t->webview->ExecuteScript(js.str().c_str(), nullptr);
}

enum class PageKind { Normal, Ai, Settings, Wizard, History };

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
        WaitForSingleObject(g_ai.serverProc.hProcess, 3000); // release its files before any delete
        CloseHandle(g_ai.serverProc.hProcess);
        CloseHandle(g_ai.serverProc.hThread);
        g_ai.serverProc = PROCESS_INFORMATION{};
        g_ai.serverStarted = false;
    }
    g_ai.phase = 0;
    g_ai.port = 0;
    g_ai.keyActive = false;
    g_ai.downloaded = 0;
    g_ai.total = 0;
    g_ai.errorMsg.clear();
}

static void SwitchAiModel(const std::wstring& id) {
    if (g_settings.aiModel == id) return;
    int phase = g_ai.phase.load();
    if (phase >= 1 && phase <= 4) return; // ignore while a setup is already in flight
    bool wasRunning = phase == 5;
    ResetAiServer();
    g_settings.aiModel = id;
    SaveSettings();
    SyncState();
    if (wasRunning && AiInstalled()) StartAiSetup();
}

// ---------------------------------------------------------------------------
// AI lifecycle. The model costs ~1 GB of RAM while loaded, so it is started on demand
// (first question, sidebar opened) instead of at launch, and — unless the user turns
// "free memory when idle" off — unloaded again after kAiIdleMs without a request.
// Requests go straight from Minima's pages to the local server, so the pages report
// each use with a small message ("aiuse"/"sb-use"/"ai-use").
// ---------------------------------------------------------------------------
static const UINT_PTR kIdleTimerId = 4;          // 1-minute housekeeping tick (AI idle + tab sleep)
static const ULONGLONG kAiIdleMs = 10 * 60 * 1000;
static const ULONGLONG kTabSleepMs = 10 * 60 * 1000;
static void PushSidebarState();

static void NoteAiUse() { g_ai.lastUse = GetTickCount64(); }

/// Starts the already-installed engine if it isn't running. Never starts a download —
/// that stays an explicit choice on the AI page / sidebar setup screen.
static void EnsureAi() {
    if (!g_settings.aiEnabled) return;
    NoteAiUse();
    if (g_ai.phase.load() == 0 && AiInstalled()) StartAiSetup();
}

/// Tells every AI surface the engine state changed outside a setup run (unload, delete).
static void BroadcastAiState() {
    SyncState();
    PushAiStatus();
    PushSidebarState();
}

static void UnloadIdleAi() {
    if (!g_settings.aiIdleUnload || g_ai.phase.load() != 5) return;
    if (GetTickCount64() - g_ai.lastUse < kAiIdleMs) return;
    ResetAiServer();
    BroadcastAiState();
}

/// Suspends background tabs nobody has looked at for kTabSleepMs. WebView2 refuses
/// (and we leave alone) tabs that are playing audio; a suspended tab resumes in Relayout.
static void SleepIdleTabs() {
    if (!g_settings.sleepTabs) return;
    ULONGLONG now = GetTickCount64();
    for (auto& t : g_app.tabs) {
        if (t->id == g_app.activeId || !t->webview || t->suspended || t->audio || t->loading) continue;
        if (now - t->lastActive < kTabSleepMs) continue;
        ComPtr<ICoreWebView2_3> wv3;
        if (FAILED(t->webview.As(&wv3)) || !wv3) continue;
        std::wstring id = t->id;
        wv3->TrySuspend(Callback<ICoreWebView2TrySuspendCompletedHandler>(
                            [id](HRESULT hr, BOOL ok) -> HRESULT {
                                Tab* tab = FindTab(id);
                                if (tab && SUCCEEDED(hr) && ok && id != g_app.activeId) {
                                    tab->suspended = true;
                                    ScheduleSync();
                                }
                                return S_OK;
                            })
                            .Get());
    }
}

/// Removes the downloaded engine and models (Settings > On-device AI).
static void DeleteAiFiles() {
    int phase = g_ai.phase.load();
    if (phase >= 1 && phase <= 4) return; // a download/start is writing these files
    ResetAiServer();
    std::error_code ec;
    std::filesystem::remove_all(BinDir(), ec);
    std::filesystem::remove_all(ModelsDir(), ec);
    BroadcastAiState();
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
    std::wstring memoryStatus = L"Memory is off.";
    if (g_settings.memory && MemoryOpen()) {
        long long n = g_memory.Count();
        memoryStatus = std::to_wstring(n) + (n == 1 ? L" page" : L" pages") + L" remembered.";
    }
    std::wstringstream memoryExcl;
    for (auto& h : MemoryExcludedHosts())
        memoryExcl << L"<div class='extrow'><span class='nm'>Never remembered: " << HtmlEscape(h)
                   << L"</span><button class='mini rm' data-host=\"" << HtmlEscape(h) << L"\">Remove</button></div>";
    bool gpuUpgrade = false;
    std::wstring gpuStatus;
    if (!VulkanAvailable()) {
        gpuStatus = L"No compatible graphics driver (Vulkan) was found, so AI runs on the processor.";
    } else if (FileExists(EnginePath()) && !EngineHasVulkan()) {
        gpuStatus = L"Your AI engine was installed before GPU support. Get the GPU engine so page summaries start sooner.";
        gpuUpgrade = g_settings.aiGpu && !setupBusy;
    } else {
        gpuStatus = L"The GPU helps read the page, so summaries and answers about it start sooner; the answer itself"
                    L" is still written on the processor. Turn off if your graphics driver misbehaves.";
        if (g_ai.phase.load() == 5)
            gpuStatus += g_ai.gpuActive.load() ? L" <b>Active.</b>" : L" <b>Not active right now</b> (fell back to the processor).";
    }
    unsigned long long aiBytes = 0; // engine + models on disk, shown on the delete button
    for (const std::wstring& dir : {BinDir(), ModelsDir()}) {
        std::error_code ec;
        for (auto it = std::filesystem::recursive_directory_iterator(dir, ec);
             !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
            if (it->is_regular_file(ec)) aiBytes += it->file_size(ec);
    }
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
      the last time Minima closed. Restored tabs load when you first open them.</div></span>
      <input type="checkbox" id="restore")HTML"
         << (g_settings.restoreSession ? L" checked" : L"") << LR"HTML(></label>
    <label class="switch"><span>Save and fill passwords<div class="sub">Offers to save sign-ins and fills them
      next time. Kept in Minima's profile on this device, encrypted by Windows for your account.</div></span>
      <input type="checkbox" id="savePasswords")HTML"
         << (g_settings.savePasswords ? L" checked" : L"") << LR"HTML(></label>
  </section>
  <section>
    <h2>Memory</h2>
    <label class="switch"><span>Remember pages I read<div class="sub">Keeps the text of pages you read so you can
      search them later from the address bar or History (Ctrl+H). Stored only on this device; sign-in, payment,
      email and chat pages are never kept.</div></span>
      <input type="checkbox" id="memory")HTML"
         << (g_settings.memory ? L" checked" : L"") << LR"HTML(></label>
    <p class="hint" style="padding:0 10px">)HTML"
         << memoryStatus << LR"HTML(</p>
    <div id="memExcl">)HTML"
         << memoryExcl.str() << LR"HTML(</div>
    <div class="actions" style="margin-top:8px">
      <button class="btn" id="openHistory">Search history &amp; memory</button>
      <button class="btn danger" id="memClear">Forget everything</button>
    </div>
  </section>
  <section>
    <h2>Performance</h2>
    <label class="switch"><span>Sleep inactive tabs<div class="sub">Tabs you haven't looked at for 10 minutes
      are suspended to save memory and battery. Tabs playing audio stay awake.</div></span>
      <input type="checkbox" id="sleepTabs")HTML"
         << (g_settings.sleepTabs ? L" checked" : L"") << LR"HTML(></label>
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
    <h2>On-device AI</h2>
    <label class="switch"><span>Enable on-device AI<div class="sub">Turns every AI feature on or off: answers
      in the address bar, the page sidebar, AI chat and "Ask Minima AI". Nothing is ever sent to the cloud.</div></span>
      <input type="checkbox" id="aiEnabled")HTML"
         << (g_settings.aiEnabled ? L" checked" : L"") << LR"HTML(></label>
    <label class="switch"><span>Free memory when idle<div class="sub">The model uses about 1&nbsp;GB of memory while
      loaded. Unload it after 10 minutes unused; it starts again (in a few seconds) the next time you ask.</div></span>
      <input type="checkbox" id="aiIdle")HTML"
         << (g_settings.aiIdleUnload ? L" checked" : L"") << LR"HTML(></label>
    <label class="switch"><span>Use the graphics chip to read pages faster<div class="sub">)HTML"
         << gpuStatus << LR"HTML(</div></span>
      <input type="checkbox" id="aiGpu")HTML"
         << (g_settings.aiGpu ? L" checked" : L"") << (VulkanAvailable() ? L"" : L" disabled") << LR"HTML(></label>)HTML"
         << (gpuUpgrade ? L"<div class='actions' style='padding:2px 10px 6px'><button class='btn' id='aiGpuEngine'>"
                          L"Download the GPU engine (~36&nbsp;MB)</button></div>"
                        : L"")
         << LR"HTML(
    <div id="models" style="margin-top:8px">)HTML"
         << models.str() << LR"HTML(</div>
    <p id="note"></p>
    <div class="actions">
      <button class="btn danger" id="aiDelete")HTML"
         << ((setupBusy || aiBytes == 0) ? L" disabled" : L"") << LR"HTML(>Delete downloaded AI files)HTML"
         << (aiBytes > 0 ? L" (" + std::to_wstring((aiBytes + (1 << 19)) >> 20) + L" MB)" : L"") << LR"HTML(</button>
    </div>
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
    <h2>Import from another browser</h2>
    <div class="actions" id="imports">)HTML"
         << ImportButtonsHtml() << LR"HTML(</div>
    <p class="hint" id="importNote">Brings over bookmarks (keeping their folders) and recent history.
      Passwords stay where they are &mdash; export them from that browser if you need them.</p>
  </section>
  <section>
    <h2>Data</h2>
    <div class="actions">
      <button class="btn danger" id="clearHistory">Clear history &amp; memory</button>
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
  document.getElementById('memory').addEventListener('change', (e) => send('set-memory', e.target.checked ? '1' : '0'));
  document.getElementById('savePasswords').addEventListener('change', (e) => send('set-passwords', e.target.checked ? '1' : '0'));
  document.getElementById('aiGpu').addEventListener('change', (e) => send('set-ai-gpu', e.target.checked ? '1' : '0'));
  if (document.getElementById('aiGpuEngine')) document.getElementById('aiGpuEngine').onclick = (e) => {
    e.target.disabled = true; e.target.textContent = 'Downloading…'; send('ai-gpu-engine');
  };
  document.getElementById('openHistory').onclick = () => send('open-history');
  document.getElementById('memClear').onclick = () => {
    if (confirm('Forget every page Memory has kept? Your history list stays.')) send('mem-clear');
  };
  document.getElementById('memExcl').addEventListener('click', (e) => {
    const b = e.target.closest('button[data-host]'); if (b) send('mem-unexclude', b.dataset.host);
  });
  document.getElementById('imports').addEventListener('click', (e) => {
    const b = e.target.closest('button.imp'); if (!b) return;
    document.querySelectorAll('button.imp').forEach(x => x.disabled = true);
    document.getElementById('importNote').textContent = 'Importing from ' + b.textContent + '…';
    send('import', b.dataset.src);
  });
  window.__importDone = (id, text, ok) => {
    document.getElementById('importNote').textContent = text;
    document.querySelectorAll('button.imp').forEach(x => x.disabled = false);
  };
  document.getElementById('sleepTabs').addEventListener('change', (e) => {
    send('set-sleep-tabs', e.target.checked ? '1' : '0');
  });
  document.getElementById('aiEnabled').addEventListener('change', (e) => {
    send('set-ai-enabled', e.target.checked ? '1' : '0');
  });
  document.getElementById('aiIdle').addEventListener('change', (e) => {
    send('set-ai-idle', e.target.checked ? '1' : '0');
  });
  document.getElementById('aiDelete').onclick = () => {
    if (confirm('Delete the downloaded AI engine and models? AI features will offer to download them again.'))
      send('ai-delete');
  };
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
    if (confirm('Clear all browsing history and everything Memory kept? This cannot be undone.')) send('clear-history');
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
                        // WebView2's own component extensions (clipboard helper, PDF viewer)
                        // aren't the user's to manage — removing the PDF viewer breaks PDFs.
                        if (info.id == L"dgiklkfkllikcanfonkcabmbdfmgleag" ||
                            info.id == L"mhjfbmdgcfjbbpaeojofohoefgiehjai")
                            continue;
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

struct ExtWork { std::wstring id, name, repo; std::string match; };

/// Worker thread: resolve → download → extract → locate manifest, then hand the folder
/// back to the UI thread (AddBrowserExtension is a profile/COM call and must run there).
static DWORD WINAPI ExtInstallThread(LPVOID p) {
    std::unique_ptr<ExtWork> w(static_cast<ExtWork*>(p));
    auto post = [](bool ok, const std::wstring& payload) {
        PostMessageW(g_app.hwnd, WM_APP_EXT_DONE, 0,
                     reinterpret_cast<LPARAM>(new std::pair<bool, std::wstring>(ok, payload)));
    };
    std::wstring url = ResolveGithubAsset(w->repo, w->match, true);
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
    auto* w = new ExtWork{found->id, found->name, found->repo, found->assetMatch};
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
    } else if (cmd == L"set-memory") {
        SetMemoryEnabled(arg != L"0");
        OpenSettings(true);
    } else if (cmd == L"mem-clear") {
        if (MemoryOpen()) g_memory.Clear();
        OpenSettings(true);
    } else if (cmd == L"mem-unexclude") {
        std::wstring kept;
        for (auto& h : MemoryExcludedHosts())
            if (h != arg) kept += (kept.empty() ? L"" : L"\n") + h;
        g_settings.memoryExclude = kept;
        SaveSettings();
        OpenSettings(true);
    } else if (cmd == L"open-history") {
        CreateTab(L"", BuildHistoryHtml(), PageKind::History);
    } else if (cmd == L"set-passwords") {
        g_settings.savePasswords = arg != L"0";
        SaveSettings();
        for (auto& t : g_app.tabs) ApplyTabSettings(t.get());
    } else if (cmd == L"set-ai-gpu") {
        g_settings.aiGpu = arg != L"0";
        SaveSettings();
        if (g_ai.phase.load() == 5) { ResetAiServer(); BroadcastAiState(); } // restarts on next use with the new mode
        OpenSettings(true);
    } else if (cmd == L"ai-gpu-engine") {
        // Swap a CPU-only engine for the Vulkan build: drop the old engine (the model stays)
        // and run setup, which downloads the right build for this machine.
        int phase = g_ai.phase.load();
        if (phase >= 1 && phase <= 4) return;
        ResetAiServer();
        std::error_code ec;
        std::filesystem::remove_all(BinDir(), ec);
        StartAiSetup();
        BroadcastAiState();
    } else if (cmd == L"import") {
        StartImport(arg);
    } else if (cmd == L"set-sleep-tabs") {
        g_settings.sleepTabs = arg != L"0";
        SaveSettings();
    } else if (cmd == L"set-ai-enabled") {
        g_settings.aiEnabled = arg != L"0";
        SaveSettings();
        if (!g_settings.aiEnabled) {
            int phase = g_ai.phase.load();
            if (phase == 0 || phase >= 5) ResetAiServer(); // an in-flight download is left to finish
            if (g_app.sidebarOpen) { g_app.sidebarOpen = false; Relayout(); }
        }
        BroadcastAiState();
    } else if (cmd == L"set-ai-idle") {
        g_settings.aiIdleUnload = arg != L"0";
        SaveSettings();
        NoteAiUse(); // start the idle clock from now
    } else if (cmd == L"ai-delete") {
        DeleteAiFiles();
        OpenSettings(true); // refresh sizes / "downloaded" badges
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
        if (MemoryOpen()) g_memory.Clear();
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
    } else if (cmd == L"wiz-memory") {
        g_settings.memory = arg == L"1";
        g_settings.memoryAsked = true;
    } else if (cmd == L"wiz-import") {
        StartImport(arg);
    } else if (cmd == L"wiz-done") {
        SaveSettings();
        SyncState();
        if (t && t->webview) {
            t->isWizardPage = false;
            t->isStartPage = true;
            t->title = L"New Tab";
            t->webview->NavigateToString(BuildStartHtml().c_str());
            if (g_app.chromeController && g_app.chromeWebview) {
                g_app.chromeController->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
                g_app.chromeWebview->ExecuteScript(L"document.getElementById('addr').focus()", nullptr);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Import from another browser (bookmarks + history; see core/importer.h for formats).
// ---------------------------------------------------------------------------
struct BrowserSource {
    const wchar_t* id;
    const wchar_t* name;
    bool firefox;
    const wchar_t* dir; // under %LOCALAPPDATA% (Chromium family) or %APPDATA% (Firefox)
};
static const BrowserSource kBrowserSources[] = {
    {L"edge", L"Microsoft Edge", false, L"Microsoft\\Edge\\User Data"},
    {L"chrome", L"Google Chrome", false, L"Google\\Chrome\\User Data"},
    {L"brave", L"Brave", false, L"BraveSoftware\\Brave-Browser\\User Data"},
    {L"vivaldi", L"Vivaldi", false, L"Vivaldi\\User Data"},
    {L"firefox", L"Mozilla Firefox", true, L"Mozilla\\Firefox"},
};
static const UINT WM_APP_IMPORT_DONE = WM_APP + 2; // lParam = new ImportResult*
static std::atomic<bool> g_importBusy{false};

static std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p)) && p) out = p;
    CoTaskMemFree(p);
    return out;
}

static std::string ReadFileBytes(const std::wstring& path) {
    std::string data;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return data;
    LARGE_INTEGER size{};
    if (GetFileSizeEx(h, &size) && size.QuadPart > 0 && size.QuadPart < (64LL << 20)) {
        data.resize(static_cast<size_t>(size.QuadPart));
        DWORD got = 0;
        if (!ReadFile(h, data.data(), static_cast<DWORD>(data.size()), &got, nullptr)) got = 0;
        data.resize(got);
    }
    CloseHandle(h);
    return data;
}

/// The browser's profile folder holding its data, or "" if it isn't installed / used.
static std::wstring SourceProfileDir(const BrowserSource& s) {
    if (s.firefox) {
        std::wstring base = KnownFolder(FOLDERID_RoamingAppData) + L"\\" + s.dir;
        bool relative = true;
        std::wstring p = Utf8ToWide(ParseFirefoxDefaultProfile(ReadFileBytes(base + L"\\profiles.ini"), relative));
        if (p.empty()) return L"";
        std::replace(p.begin(), p.end(), L'/', L'\\');
        std::wstring dir = relative ? base + L"\\" + p : p;
        return FileExists(dir + L"\\places.sqlite") ? dir : L"";
    }
    std::wstring base = KnownFolder(FOLDERID_LocalAppData) + L"\\" + s.dir;
    if (FileExists(base + L"\\Default\\Bookmarks") || FileExists(base + L"\\Default\\History")) return base + L"\\Default";
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((base + L"\\Profile *").c_str(), &fd);
    std::wstring found;
    if (f != INVALID_HANDLE_VALUE) {
        do {
            std::wstring dir = base + L"\\" + fd.cFileName;
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && FileExists(dir + L"\\History")) { found = dir; break; }
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    return found;
}

/// Opens another browser's (probably locked) SQLite file read-only: from a private copy
/// when the file can be copied, else in place with SQLite's immutable mode.
static sqlite3* OpenForeignDb(const std::wstring& path, const std::wstring& tmpDir, std::vector<std::wstring>& cleanup) {
    std::wstring copy = tmpDir + L"\\" + std::to_wstring(cleanup.size()) + L".db";
    sqlite3* db = nullptr;
    if (CopyFileW(path.c_str(), copy.c_str(), FALSE)) {
        cleanup.push_back(copy);
        if (CopyFileW((path + L"-wal").c_str(), (copy + L"-wal").c_str(), FALSE)) cleanup.push_back(copy + L"-wal");
        cleanup.push_back(copy + L"-shm");
        if (sqlite3_open_v2(WideToUtf8(copy).c_str(), &db, SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK) return db;
        sqlite3_close(db);
        db = nullptr;
    }
    std::string uri = "file:///";
    for (char c : WideToUtf8(path)) {
        if (c == '\\') uri += '/';
        else if (c == ' ' || c == '#' || c == '?' || c == '%') { char b[4]; snprintf(b, 4, "%%%02X", (unsigned char)c); uri += b; }
        else uri += c;
    }
    uri += "?immutable=1";
    if (sqlite3_open_v2(uri.c_str(), &db, SQLITE_OPEN_READONLY | SQLITE_OPEN_URI, nullptr) == SQLITE_OK) return db;
    sqlite3_close(db);
    return nullptr;
}

struct ImportJob { std::wstring id, name, profile; bool firefox; };
struct ImportResult {
    std::wstring id, name, error;
    std::vector<BookmarkEntry> bookmarks;
    std::vector<HistoryEntry> history;
};

static DWORD WINAPI ImportThread(LPVOID p) {
    std::unique_ptr<ImportJob> job(static_cast<ImportJob*>(p));
    auto* r = new ImportResult{job->id, job->name, L"", {}, {}};
    std::wstring tmp = g_dataDir + L"\\import-tmp";
    CreateDirectoryW(tmp.c_str(), nullptr);
    std::vector<std::wstring> cleanup;
    if (job->firefox) {
        if (sqlite3* db = OpenForeignDb(job->profile + L"\\places.sqlite", tmp, cleanup)) {
            r->bookmarks = ReadFirefoxBookmarks(db);
            r->history = ReadFirefoxHistory(db, 5000);
            sqlite3_close(db);
        }
    } else {
        r->bookmarks = ParseChromiumBookmarks(ReadFileBytes(job->profile + L"\\Bookmarks"));
        if (sqlite3* db = OpenForeignDb(job->profile + L"\\History", tmp, cleanup)) {
            r->history = ReadChromiumHistory(db, 5000);
            sqlite3_close(db);
        }
    }
    for (auto& f : cleanup) DeleteFileW(f.c_str());
    RemoveDirectoryW(tmp.c_str());
    if (r->bookmarks.empty() && r->history.empty())
        r->error = L"Couldn't read anything from " + job->name + L". If it's open, close it and try again.";
    PostMessageW(g_app.hwnd, WM_APP_IMPORT_DONE, 0, reinterpret_cast<LPARAM>(r));
    return 0;
}

static void StartImport(const std::wstring& id) {
    for (auto& s : kBrowserSources) {
        if (id != s.id) continue;
        std::wstring profile = SourceProfileDir(s);
        if (profile.empty() || g_importBusy.exchange(true)) return;
        auto* job = new ImportJob{s.id, s.name, profile, s.firefox};
        HANDLE h = CreateThread(nullptr, 0, ImportThread, job, 0, nullptr);
        if (h) CloseHandle(h);
        else { delete job; g_importBusy = false; }
        return;
    }
}

/// Buttons for each browser found on this PC (shared by the wizard and Settings).
static std::wstring ImportButtonsHtml() {
    std::wstringstream ss;
    int n = 0;
    for (auto& s : kBrowserSources) {
        if (SourceProfileDir(s).empty()) continue;
        ss << L"<button class='btn imp' data-src='" << s.id << L"'>" << s.name << L"</button>";
        n++;
    }
    if (!n) ss << L"<span class='hint'>No other browsers found on this PC.</span>";
    return ss.str();
}

/// Merges a finished import and tells the wizard/settings page how it went.
static void FinishImport(ImportResult* r) {
    g_importBusy = false;
    std::wstring text = r->error;
    if (text.empty()) {
        int b = MergeBookmarks(g_bookmarks, r->bookmarks);
        int h = MergeHistory(g_history, r->history, kMaxHistory);
        SaveBookmarks();
        SaveHistory();
        SyncState();
        text = L"Imported " + std::to_wstring(b) + L" bookmark" + (b == 1 ? L"" : L"s") + L" and " + std::to_wstring(h) +
               L" history entr" + (h == 1 ? L"y" : L"ies") + L" from " + r->name + L".";
        if (b == 0 && h == 0) text = L"Everything from " + r->name + L" was already in Minima.";
    }
    std::wstring js = L"window.__importDone && window.__importDone(\"" + JsonEscape(r->id) + L"\",\"" + JsonEscape(text) +
                      L"\"," + (r->error.empty() ? L"true" : L"false") + L")";
    for (auto& t : g_app.tabs)
        if ((t->isWizardPage || t->isSettingsPage) && t->webview) t->webview->ExecuteScript(js.c_str(), nullptr);
}

// ---------------------------------------------------------------------------
// Commands from the start page and the History & Memory page.
// ---------------------------------------------------------------------------
static void SetMemoryEnabled(bool on) {
    g_settings.memory = on;
    g_settings.memoryAsked = true;
    SaveSettings();
    SyncState(); // the palette offers memory search only while it's on
}

static void HandleStartCommand(const std::wstring& msg) {
    std::wstring cmd = msg, arg;
    size_t sep = msg.find(L'\x1F');
    if (sep != std::wstring::npos) { cmd = msg.substr(0, sep); arg = msg.substr(sep + 1); }
    if (cmd == L"start-bookmarks") CreateTab(L"", BuildBookmarksHtml());
    else if (cmd == L"start-memory") SetMemoryEnabled(arg == L"1");
}

static void HandleHistoryCommand(Tab* t, const std::wstring& msg) {
    std::wstring cmd = msg, arg;
    size_t sep = msg.find(L'\x1F');
    if (sep != std::wstring::npos) { cmd = msg.substr(0, sep); arg = msg.substr(sep + 1); }
    if (cmd == L"hist-search") {
        std::wstring js = L"window.__results && window.__results(" + HistorySearchJson(arg) + L")";
        if (t->webview) t->webview->ExecuteScript(js.c_str(), nullptr);
    } else if (cmd == L"hist-remove") {
        g_history.erase(std::remove_if(g_history.begin(), g_history.end(), [&](const HistoryEntry& h) { return h.url == arg; }),
                        g_history.end());
        ScheduleSaveHistory();
        if (MemoryOpen()) g_memory.Forget(arg); // removing a visit also removes what was remembered
    } else if (cmd == L"mem-forget") {
        if (MemoryOpen()) g_memory.Forget(arg);
    } else if (cmd == L"mem-exclude") {
        std::wstring host = arg;
        std::transform(host.begin(), host.end(), host.begin(), towlower);
        if (host.empty() || host.find(L'\n') != std::wstring::npos) return;
        auto list = MemoryExcludedHosts();
        if (std::find(list.begin(), list.end(), host) == list.end()) {
            g_settings.memoryExclude += (g_settings.memoryExclude.empty() ? L"" : L"\n") + host;
            SaveSettings();
        }
        if (MemoryOpen()) g_memory.ForgetSite(host);
    } else if (cmd == L"mem-on") {
        SetMemoryEnabled(true);
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
            case 'H': CreateTab(L"", BuildHistoryHtml(), PageKind::History); return true;
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
    if (key == VK_F9) { ToggleReader(); return true; }
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
            [id](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
                if (Tab* t = FindTab(id)) {
                    t->loading = true;
                    t->blocked = 0;
                    t->favicon.clear();
                    t->readerable = false;
                    t->extractDue = 0;
                    CoStr uri;
                    if (args && SUCCEEDED(args->get_Uri(&uri)) && uri && _wcsnicmp(uri.get(), L"http", 4) == 0)
                        t->isStartPage = false; // left the new-tab page for a real site
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
                    if (success && (t->url.rfind(L"https://", 0) == 0 || t->url.rfind(L"http://", 0) == 0))
                        ScheduleExtract(t); // reader availability + Memory
                    SyncState();
                    if (id == g_app.activeId) RefreshSidebar();
                }
                return S_OK;
            })
            .Get(),
        &tok);

    // Messages from page content are only honored for our own internal pages — arbitrary
    // websites can call postMessage too. Two gates: the tab's page flag AND the sender being
    // NavigateToString content, which WebView2 reports as exactly "about:blank". A website
    // can't run script in such a document here (popups get their own unlinked webview,
    // iframes have no chrome.webview), so a tab that navigated away from an internal page —
    // its flag still set — gains a website nothing.
    wv->add_WebMessageReceived(
        Callback<ICoreWebView2WebMessageReceivedEventHandler>(
            [id](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                Tab* t = FindTab(id);
                if (!t || !args) return S_OK;
                CoStr msg;
                if (!SUCCEEDED(args->TryGetWebMessageAsString(&msg)) || !msg) return S_OK;
                // Reader view runs inside the website's page, so its preference updates are
                // the one message accepted from any page — validated, and only font/size/theme.
                if (wcsncmp(msg.get(), L"reader-prefs\x1F", 13) == 0) {
                    ApplyReaderPrefs(g_settings, msg.get() + 13);
                    SaveSettings();
                    return S_OK;
                }
                CoStr source;
                bool internal = SUCCEEDED(args->get_Source(&source)) && source && wcscmp(source.get(), L"about:blank") == 0;
                if (!internal) return S_OK;
                if (t->isStartPage) {
                    HandleStartCommand(msg.get());
                } else if (t->isHistoryPage) {
                    HandleHistoryCommand(t, msg.get());
                } else if (t->isAiPage && wcscmp(msg.get(), L"ai-start") == 0) {
                    if (g_settings.aiEnabled) { NoteAiUse(); StartAiSetup(); }
                } else if (t->isAiPage && wcscmp(msg.get(), L"ai-use") == 0) {
                    NoteAiUse();
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
                    if (!g_settings.aiEnabled) return S_OK;
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
                                EnsureAi();
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
                // Our CreateTab navigation is browser-initiated, which Chromium would allow even
                // to data:/javascript: URLs a page could never open itself — refuse those.
                if (SUCCEEDED(args->get_Uri(&uri)) && uri && wcsncmp(uri.get(), L"data:", 5) != 0 &&
                    _wcsnicmp(uri.get(), L"javascript:", 11) != 0)
                    CreateTab(uri.get());
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
    tab->isHistoryPage = kind == PageKind::History;
    tab->isStartPage = navigateTo.empty() && htmlContent.empty();
    Tab* raw = tab.get();
    g_app.tabs.push_back(std::move(tab));
    g_app.activeId = id;
    SyncState();
    MaterializeTab(raw, navigateTo, htmlContent);
}

/// Adds a restored tab that only gets a webview when it is first shown (see Relayout).
static void AddLazyTab(const std::wstring& url, const std::wstring& title) {
    auto tab = std::make_unique<Tab>();
    tab->id = L"t" + std::to_wstring(g_app.nextTabNum++);
    tab->url = url;
    tab->pendingUrl = url;
    tab->title = title.empty() ? url : title; // sessions saved before titles were recorded
    g_app.tabs.push_back(std::move(tab));
}

/// Per-webview settings that follow Minima's own: WebView2's built-in password manager
/// (off by default in WebView2; saved logins are encrypted by the OS for this user, in the
/// profile) and form autofill.
static void ApplyTabSettings(Tab* t) {
    ComPtr<ICoreWebView2Settings> s;
    ComPtr<ICoreWebView2Settings4> s4;
    if (!t->webview || FAILED(t->webview->get_Settings(&s)) || !s || FAILED(s.As(&s4)) || !s4) return;
    s4->put_IsPasswordAutosaveEnabled(g_settings.savePasswords ? TRUE : FALSE);
    s4->put_IsGeneralAutofillEnabled(TRUE);
}

/// Creates the WebView2 controller for `tab` and loads its content.
static void MaterializeTab(Tab* tab, const std::wstring& navigateTo, const std::wstring& htmlContent) {
    if (!g_app.env) return;
    tab->creating = true;
    std::wstring id = tab->id;
    g_app.env->CreateCoreWebView2Controller(
        g_app.hwnd,
        Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
            [id, navigateTo, htmlContent](HRESULT hr, ICoreWebView2Controller* controller) -> HRESULT {
                Tab* raw = FindTab(id);
                if (raw) raw->creating = false;
                if (FAILED(hr) || !controller) {
                    wchar_t buf[128];
                    swprintf(buf, 128, L"Failed to create tab webview (0x%08lx)", hr);
                    MessageBoxW(g_app.hwnd, buf, L"Minima", MB_ICONERROR);
                    return hr;
                }
                if (!raw) { controller->Close(); return S_OK; } // tab closed while creating
                raw->controller = controller;
                controller->get_CoreWebView2(&raw->webview);
                ApplyTabSettings(raw);
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
    // Memory: pages whose *text* matches, for queries that look like words, not addresses.
    bool wordy = q.size() >= 3 && q.find(L"://") == std::wstring::npos &&
                 (q.find(L' ') != std::wstring::npos || q.find(L'.') == std::wstring::npos);
    if (g_settings.memory && wordy && MemoryOpen()) {
        int shown = 0;
        for (auto& m : g_memory.Search(q, 6)) {
            if (shown >= 3 || std::find(seen.begin(), seen.end(), m.url) != seen.end()) continue;
            seen.push_back(m.url);
            ss << (first ? L"" : L",") << L"{\"kind\":\"m\",\"title\":\"" << JsonEscape(m.title) << L"\",\"url\":\""
               << JsonEscape(m.url) << L"\",\"snip\":\"" << JsonEscape(m.snippet) << L"\"}";
            first = false;
            shown++;
        }
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
       << L",\"" << JsonEscape(title) << L"\",\"" << JsonEscape(url) << L"\",\""
       << JsonEscape(AiKeyForPages()) << L"\"," << (AiInstalled() ? L"true" : L"false") << L")";
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
    // The article (title + main text) when there is one — far fewer tokens than the whole
    // page's innerText (menus, footers, ads), so the on-device model answers faster.
    std::wstring js = std::wstring(L"(() => { try { const R = ") + kExtractJs +
                      L"; if (R.words >= 60) return ((R.title ? R.title + '\\n\\n' : '') + R.text).slice(0, 12000); }"
                      L" catch (e) {} return (document.body ? document.body.innerText : '').slice(0, 12000); })()";
    t->webview->ExecuteScript(
        js.c_str(),
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
        if (g_settings.aiEnabled) { NoteAiUse(); StartAiSetup(); }
    } else if (msg == L"sb-use") {
        NoteAiUse();
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
    if (!g_app.sidebarOpen) EnsureAi(); // warm the model while the user reads / types
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
                // Only the tab that was active loads now; the rest wake up when first shown.
                for (auto& st : session) {
                    AddLazyTab(st.url, st.title);
                    if (st.current) g_app.activeId = g_app.tabs.back()->id;
                }
                if (g_app.activeId.empty()) g_app.activeId = g_app.tabs.front()->id;
                Relayout(); // materializes the active tab
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
        if (g_settings.aiEnabled) ToggleSidebar();
    } else if (cmd == L"reader") {
        ToggleReader();
    } else if (cmd == L"bookmarks") {
        CreateTab(L"", BuildBookmarksHtml());
    } else if (cmd == L"memsearch") {
        CreateTab(L"", BuildHistoryHtml(arg), PageKind::History);
    } else if (cmd == L"aistart") {
        EnsureAi(); // inline answer needs the (installed) engine running
    } else if (cmd == L"aiuse") {
        NoteAiUse();
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
        CreateTab(L"", BuildHistoryHtml(), PageKind::History);
    } else if (cmd == L"askai") {
        if (g_settings.aiEnabled) {
            EnsureAi();
            CreateTab(L"", BuildAiHtml(arg), PageKind::Ai);
        }
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
                            if ((ctrl || key == VK_F11 || key == VK_F9) && key != 'L' && key != 'K' && key != 'F' &&
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
                // AI was switched off while a download was finishing: don't leave it running.
                if (!g_settings.aiEnabled && g_ai.phase.load() == 5) ResetAiServer();
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
            } else if (wp == kHistoryTimerId) {
                KillTimer(hwnd, kHistoryTimerId);
                if (g_historyDirty) SaveHistory();
            } else if (wp == kIdleTimerId) {
                UnloadIdleAi();
                SleepIdleTabs();
            } else if (wp == kExtractTimerId) {
                KillTimer(hwnd, kExtractTimerId);
                RunDueExtractions();
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
        case WM_APP_IMPORT_DONE: {
            std::unique_ptr<ImportResult> r(reinterpret_cast<ImportResult*>(lp));
            FinishImport(r.get());
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
            if (g_historyDirty) SaveHistory();
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

    // MINIMA_PROFILE_DIR runs a separate, self-contained profile (development/testing). Such
    // an instance never hands off to — or blocks — the user's normal Minima window.
    std::wstring profileOverride;
    wchar_t ovBuf[MAX_PATH];
    DWORD ovLen = GetEnvironmentVariableW(L"MINIMA_PROFILE_DIR", ovBuf, MAX_PATH);
    if (ovLen > 0 && ovLen < MAX_PATH) profileOverride.assign(ovBuf, ovLen);

    // Single instance: hand the URL (if any) to the running window as a new tab —
    // link clicks from other apps must not spawn a second browser window.
    HANDLE instanceMutex =
        profileOverride.empty() ? CreateMutexW(nullptr, TRUE, L"MinimaBrowserSingleInstance") : nullptr;
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
    if (!profileOverride.empty()) userData = profileOverride;
    CreateDirectoryW(userData.c_str(), nullptr);
    g_dataDir = userData;
    g_firstRun = !FileExists(userData + L"\\settings.json");
    LoadBookmarks();
    LoadHistory();
    LoadSettings();
    // The AI normally starts on first use (see EnsureAi). Users who turned "free memory when
    // idle" off want it always ready, so warm it at launch for them.
    if (g_settings.aiEnabled && !g_settings.aiIdleUnload && AiInstalled()) StartAiSetup();
    SetTimer(g_app.hwnd, kIdleTimerId, 60 * 1000, nullptr);

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
