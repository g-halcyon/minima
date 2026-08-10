// shell.mm — Minima's macOS shell: NSWindow + WKWebView hosting the SAME portable core
// (src/core/) as the Windows build. The chrome strip and internal pages are the core's
// HTML assets, unchanged — an injected shim maps `window.chrome.webview.postMessage`
// onto WKScriptMessageHandler.
//
// Build (needs Xcode command line tools):
//   clang++ -std=c++17 -ObjC++ -fobjc-arc src/platform/macos/shell.mm -o minima \
//       -framework Cocoa -framework WebKit
//
// Status: code-complete port of the core browsing experience — tabs, chrome UI,
// navigation, URL/search, bookmarks, history, session restore, ad/tracker blocking
// (core host list compiled into a WKContentRuleList), popups→tabs.
// Not yet ported (documented deltas from Windows, see PORTING.md):
//   * Chrome extensions — WKWebView cannot load them (platform limitation, permanent).
//   * On-device AI (llama-server sidecar) — pages render; NSTask management TODO.
//   * Find-in-page — WKWebView find API (macOS 13+) wiring TODO; bar renders.
// This file has NOT been compiled in CI yet (developed on Windows); expect only minor
// signature fixes on first macOS build, not design changes.
#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>

#include <algorithm>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "../../core/adblock.h"
#include "../../core/json.h"
#include "../../core/models.h"
#include "../../core/storage.h"
#include "../../core/ui_assets.h"
#include "../../core/urls.h"
#include "../../core/utf.h"

using namespace minima;

struct MTab : TabModel {
    WKWebView* view = nullptr; // ARC-managed via the container below
};

static struct {
    NSWindow* window = nullptr;
    WKWebView* chrome = nullptr;
    NSView* content = nullptr;
    std::vector<std::unique_ptr<MTab>> tabs;
    std::string activeId;
    int nextTabNum = 1;
    int suggestDip = 0;
    std::string initialUrl;
    WKContentRuleList* adFilter = nullptr;
} g;

static Settings g_settings;
static std::vector<BookmarkEntry> g_bookmarks;
static std::vector<HistoryEntry> g_history;
static std::string g_dataDir;

// ---------------------------------------------------------------------------
// Storage (UTF-8 files; the core (de)serializes wide strings).
// ---------------------------------------------------------------------------
static std::wstring ReadDataFile(const std::string& name) {
    std::ifstream f(g_dataDir + "/" + name, std::ios::binary);
    if (!f) return L"";
    std::stringstream ss;
    ss << f.rdbuf();
    return Utf8ToWideStr(ss.str());
}

static void WriteDataFile(const std::string& name, const std::wstring& content) {
    std::ofstream f(g_dataDir + "/" + name, std::ios::binary | std::ios::trunc);
    if (f) {
        std::string u = WideToUtf8(content);
        f.write(u.data(), (std::streamsize)u.size());
    }
}

static NSString* NS(const std::string& s) { return [NSString stringWithUTF8String:s.c_str()]; }
static NSString* NSW(const std::wstring& s) { return NS(WideToUtf8(s)); }

// The shim that gives the core's UI `window.chrome.webview` on WebKit.
static const char* kBridgeShim =
    "window.chrome = window.chrome || {};"
    "window.chrome.webview = {"
    "  _ls: [],"
    "  postMessage: (m) => window.webkit.messageHandlers.minima.postMessage(m),"
    "  addEventListener: (t, f) => { if (t === 'message') window.chrome.webview._ls.push(f); },"
    "  __dispatch: (data) => { for (const f of window.chrome.webview._ls) f({data: data}); }"
    "};";

static void PostChromeJson(const std::string& json) {
    [g.chrome evaluateJavaScript:NS("window.chrome.webview.__dispatch(" + json + ")")
               completionHandler:nil];
}

// ---------------------------------------------------------------------------
static MTab* FindTab(const std::string& id) {
    for (auto& t : g.tabs)
        if (WideToUtf8(t->id) == id) return t.get();
    return nullptr;
}
static MTab* ActiveTab() { return FindTab(g.activeId); }

static void Relayout() {
    CGFloat W = g.content.bounds.size.width, H = g.content.bounds.size.height;
    CGFloat chromeH = 88 + std::min(g.suggestDip, 340);
    g.chrome.frame = NSMakeRect(0, H - chromeH, W, chromeH);
    for (auto& t : g.tabs) {
        t->view.hidden = (WideToUtf8(t->id) != g.activeId);
        t->view.frame = NSMakeRect(0, 0, W, H - chromeH);
    }
}

static void SaveSession() {
    std::vector<SessionTab> tabs;
    for (auto& t : g.tabs) {
        std::string url = WideToUtf8(t->url);
        if (url.empty() || url.rfind("about:", 0) == 0 || url.rfind("data:", 0) == 0) continue;
        tabs.push_back({t->url, WideToUtf8(t->id) == g.activeId});
    }
    WriteDataFile("session.json", SerializeSession(tabs));
}

static void SyncState() {
    std::wstringstream ss;
    ss << L"{\"active\":\"" << JsonEscape(Utf8ToWideStr(g.activeId))
       << L"\",\"aiPort\":0,\"aiPhase\":0,\"adblock\":" << (g_settings.adblock ? L"true" : L"false")
       << L",\"tabs\":[";
    bool first = true;
    for (auto& t : g.tabs) {
        if (!first) ss << L",";
        first = false;
        std::wstring url = t->url.rfind(L"data:", 0) == 0 ? L"" : t->url;
        bool bm = false;
        for (auto& b : g_bookmarks)
            if (b.url == t->url) bm = true;
        ss << L"{\"id\":\"" << JsonEscape(t->id) << L"\",\"title\":\"" << JsonEscape(t->title)
           << L"\",\"url\":\"" << JsonEscape(url) << L"\",\"fav\":\"\",\"blocked\":" << t->blocked
           << L",\"audio\":false,\"muted\":false,\"loading\":" << (t->loading ? L"true" : L"false")
           << L",\"canBack\":" << (t->canBack ? L"true" : L"false") << L",\"canFwd\":"
           << (t->canFwd ? L"true" : L"false") << L",\"bookmarked\":" << (bm ? L"true" : L"false")
           << L"}";
    }
    ss << L"]}";
    PostChromeJson(WideToUtf8(ss.str()));
    if (MTab* t = ActiveTab())
        g.window.title = t->title.empty() ? @"Minima" : NSW(t->title + L" — Minima");
    SaveSession();
}

static std::wstring BuildStartHtml() {
    std::wstringstream tiles;
    for (auto& b : g_bookmarks) {
        std::wstring label = b.title.empty() ? b.url : b.title;
        wchar_t initial = label.empty() ? L'?' : towupper(label[0]);
        tiles << L"<a class='tile' href='" << JsonEscape(b.url) << L"'><div class='fav'>" << initial
              << L"</div><span>" << JsonEscape(label) << L"</span></a>";
    }
    return kStartHtmlHead + tiles.str() + kStartHtmlTail;
}

static std::wstring BuildHistoryHtml() {
    std::wstringstream rows;
    if (g_history.empty()) rows << L"<p class='empty'>No history yet.</p>";
    for (auto& h : g_history)
        rows << L"<a class='row' href='" << JsonEscape(h.url) << L"'><span class='t'>"
             << JsonEscape(h.title.empty() ? h.url : h.title) << L"</span><span class='u'>"
             << JsonEscape(h.url) << L"</span></a>";
    return kHistoryHtmlHead + rows.str() + kHistoryHtmlTail;
}

static void CreateTab(const std::string& navigateTo, const std::wstring& htmlContent = L"");
static void HandleUiCommand(const std::string& msg);

// ---------------------------------------------------------------------------
// Delegates.
// ---------------------------------------------------------------------------
@interface MinimaBridge : NSObject <WKScriptMessageHandler, WKNavigationDelegate, WKUIDelegate>
@end
@implementation MinimaBridge
- (void)userContentController:(WKUserContentController*)ucc
      didReceiveScriptMessage:(WKScriptMessage*)message {
    if ([message.body isKindOfClass:[NSString class]])
        HandleUiCommand(std::string([(NSString*)message.body UTF8String]));
}
- (void)webView:(WKWebView*)webView didFinishNavigation:(WKNavigation*)nav {
    for (auto& t : g.tabs) {
        if (t->view != webView) continue;
        t->loading = false;
        t->title = Utf8ToWideStr(webView.title ? webView.title.UTF8String : "");
        t->url = Utf8ToWideStr(webView.URL ? webView.URL.absoluteString.UTF8String : "");
        t->canBack = webView.canGoBack;
        t->canFwd = webView.canGoForward;
        if (!t->url.empty() && t->url.rfind(L"about:", 0) != 0 && t->url.rfind(L"data:", 0) != 0) {
            auto it = std::find_if(g_history.begin(), g_history.end(),
                                   [&](const HistoryEntry& h) { return h.url == t->url; });
            if (it != g_history.end()) g_history.erase(it);
            g_history.insert(g_history.begin(),
                             {t->url, t->title, (long long)[NSDate date].timeIntervalSince1970});
            if (g_history.size() > 5000) g_history.resize(5000);
            WriteDataFile("history.json", SerializeHistory(g_history));
        }
        SyncState();
        return;
    }
}
- (void)webView:(WKWebView*)webView
    didStartProvisionalNavigation:(WKNavigation*)nav {
    for (auto& t : g.tabs)
        if (t->view == webView) { t->loading = true; SyncState(); return; }
}
// Popups / target=_blank open as tabs.
- (WKWebView*)webView:(WKWebView*)webView
    createWebViewWithConfiguration:(WKWebViewConfiguration*)config
               forNavigationAction:(WKNavigationAction*)action
                    windowFeatures:(WKWindowFeatures*)features {
    if (action.request.URL) CreateTab(action.request.URL.absoluteString.UTF8String);
    return nil;
}
@end

static MinimaBridge* g_bridge;

// ---------------------------------------------------------------------------
static void CreateTab(const std::string& navigateTo, const std::wstring& htmlContent) {
    auto tab = std::make_unique<MTab>();
    tab->id = L"t" + std::to_wstring(g.nextTabNum++);
    MTab* raw = tab.get();
    g.tabs.push_back(std::move(tab));
    g.activeId = WideToUtf8(raw->id);

    WKWebViewConfiguration* cfg = [WKWebViewConfiguration new];
    if (g.adFilter && g_settings.adblock)
        [cfg.userContentController addContentRuleList:g.adFilter];
    raw->view = [[WKWebView alloc] initWithFrame:NSZeroRect configuration:cfg];
    raw->view.navigationDelegate = g_bridge;
    raw->view.UIDelegate = g_bridge;
    [g.content addSubview:raw->view];

    if (!navigateTo.empty()) {
        [raw->view loadRequest:[NSURLRequest requestWithURL:[NSURL URLWithString:NS(navigateTo)]]];
    } else if (!htmlContent.empty()) {
        [raw->view loadHTMLString:NSW(htmlContent) baseURL:nil];
    } else {
        [raw->view loadHTMLString:NSW(BuildStartHtml()) baseURL:nil];
    }
    Relayout();
    SyncState();
}

static void CloseTab(const std::string& id) {
    auto it = std::find_if(g.tabs.begin(), g.tabs.end(),
                           [&](const auto& t) { return WideToUtf8(t->id) == id; });
    if (it == g.tabs.end()) return;
    size_t idx = (size_t)(it - g.tabs.begin());
    [(*it)->view removeFromSuperview];
    g.tabs.erase(it);
    if (g.tabs.empty()) { CreateTab(""); return; }
    if (g.activeId == id) g.activeId = WideToUtf8(g.tabs[std::min(idx, g.tabs.size() - 1)]->id);
    Relayout();
    SyncState();
}

static void HandleUiCommand(const std::string& msg) {
    std::string cmd = msg, arg;
    size_t sep = msg.find('\x1F');
    if (sep != std::string::npos) { cmd = msg.substr(0, sep); arg = msg.substr(sep + 1); }
    MTab* t = ActiveTab();
    if (cmd == "ready") {
        if (!g.tabs.empty()) { SyncState(); return; }
        std::vector<SessionTab> session =
            g.initialUrl.empty() && g_settings.restoreSession ? ParseSession(ReadDataFile("session.json"))
                                                              : std::vector<SessionTab>{};
        if (!g.initialUrl.empty()) {
            CreateTab(g.initialUrl);
        } else if (!session.empty()) {
            std::string cur;
            for (auto& st : session) {
                CreateTab(WideToUtf8(st.url));
                if (st.current) cur = g.activeId;
            }
            if (!cur.empty()) g.activeId = cur;
            Relayout();
            SyncState();
        } else {
            CreateTab("");
        }
    } else if (cmd == "navigate") {
        if (!t) { CreateTab(""); t = ActiveTab(); }
        std::wstring url = ResolveInputToUrl(Utf8ToWideStr(arg),
                                             FindSearchEngine(g_settings.searchEngine).urlPrefix);
        [t->view loadRequest:[NSURLRequest requestWithURL:[NSURL URLWithString:NSW(url)]]];
    } else if (cmd == "newtab") {
        CreateTab("");
    } else if (cmd == "close") {
        CloseTab(arg);
    } else if (cmd == "select") {
        if (FindTab(arg)) { g.activeId = arg; Relayout(); SyncState(); }
    } else if (cmd == "back") {
        if (t) [t->view goBack];
    } else if (cmd == "forward") {
        if (t) [t->view goForward];
    } else if (cmd == "reload") {
        if (t) [t->view reload];
    } else if (cmd == "bookmark") {
        if (t && !t->url.empty()) {
            auto it = std::find_if(g_bookmarks.begin(), g_bookmarks.end(),
                                   [&](const BookmarkEntry& b) { return b.url == t->url; });
            if (it != g_bookmarks.end()) g_bookmarks.erase(it);
            else g_bookmarks.push_back({t->url, t->title});
            WriteDataFile("bookmarks.json", SerializeBookmarks(g_bookmarks));
            SyncState();
        }
    } else if (cmd == "history") {
        CreateTab("", BuildHistoryHtml());
    } else if (cmd == "shield") {
        g_settings.adblock = !g_settings.adblock;
        WriteDataFile("settings.json", SerializeSettings(g_settings));
        SyncState();
    } else if (cmd == "suggest") {
        g.suggestDip = atoi(arg.c_str());
        Relayout();
    }
    // Not yet ported on macOS: query suggestions, find (macOS 13 WKFindConfiguration),
    // askai/sidebar (AI sidecar), settings page, downloads — see file header.
}

// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        g_bridge = [MinimaBridge new];
        if (argc > 1) g.initialUrl = argv[1];

        g_dataDir = std::string([[NSSearchPathForDirectoriesInDomains(
                        NSApplicationSupportDirectory, NSUserDomainMask, YES) firstObject]
                        UTF8String]) + "/Minima";
        [[NSFileManager defaultManager] createDirectoryAtPath:NS(g_dataDir)
                                  withIntermediateDirectories:YES
                                                   attributes:nil
                                                        error:nil];
        ApplySettings(g_settings, ReadDataFile("settings.json"));
        g_bookmarks = ParseBookmarks(ReadDataFile("bookmarks.json"));
        g_history = ParseHistory(ReadDataFile("history.json"));

        g.window = [[NSWindow alloc]
            initWithContentRect:NSMakeRect(100, 100, 1200, 800)
                      styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                NSWindowStyleMaskResizable | NSWindowStyleMaskMiniaturizable
                        backing:NSBackingStoreBuffered
                          defer:NO];
        g.window.title = @"Minima";
        g.content = g.window.contentView;

        // Chrome strip: the core's UI + the postMessage bridge shim.
        WKWebViewConfiguration* cfg = [WKWebViewConfiguration new];
        [cfg.userContentController
            addUserScript:[[WKUserScript alloc] initWithSource:NS(kBridgeShim)
                                                 injectionTime:WKUserScriptInjectionTimeAtDocumentStart
                                              forMainFrameOnly:YES]];
        [cfg.userContentController addScriptMessageHandler:g_bridge name:@"minima"];
        g.chrome = [[WKWebView alloc] initWithFrame:NSZeroRect configuration:cfg];
        [g.content addSubview:g.chrome];

        // Ad/tracker blocking: compile the core's host list into a WKContentRuleList.
        std::string rules = "[";
        bool first = true;
        for (const wchar_t* d : kAdHosts) {
            if (!first) rules += ",";
            first = false;
            rules += R"({"trigger":{"url-filter":".*","if-domain":["*)" + WideToUtf8(d) +
                     R"("]},"action":{"type":"block"}})";
        }
        rules += "]";
        [WKContentRuleListStore.defaultStore
            compileContentRuleListForIdentifier:@"minima-ads"
                         encodedContentRuleList:NS(rules)
                              completionHandler:^(WKContentRuleList* list, NSError* err) {
                                g.adFilter = list;
                              }];

        [g.chrome loadHTMLString:NSW(std::wstring(kChromeHtml)) baseURL:nil];
        Relayout();
        [g.window makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];
        [NSApp run];
    }
    return 0;
}
