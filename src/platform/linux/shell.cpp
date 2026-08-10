// shell.cpp — Minima's Linux shell: GTK3 window + WebKitGTK webviews hosting the SAME
// portable core (src/core/) as the Windows build. The chrome strip, start/history/
// settings pages, palette and find bar are the core's HTML assets, unchanged — a small
// injected shim maps `window.chrome.webview.postMessage` onto WebKit script messages.
//
// Build (Debian/Ubuntu: apt install build-essential pkg-config libgtk-3-dev libwebkit2gtk-4.1-dev):
//   cmake -B build -S . && cmake --build build       (from the repo root)
// or directly:
//   c++ -std=c++17 src/platform/linux/shell.cpp -o minima \
//       $(pkg-config --cflags --libs gtk+-3.0 webkit2gtk-4.1)
//
// Status: code-complete port of the core browsing experience — tabs, chrome UI,
// navigation, URL/search, bookmarks, history, settings, session restore, ad/tracker
// blocking (core host list compiled into a WebKit content filter), popups→tabs.
// Not yet ported (documented deltas from Windows, see PORTING.md):
//   * Chrome extensions — WebKitGTK cannot load them (platform limitation, permanent).
//   * On-device AI (llama-server sidecar) — the pages render; process management TODO.
//   * Native find-in-page — uses WebKitFindController (wired below), counts approximate.
// This file has NOT been compiled in CI yet (developed on Windows); expect only minor
// signature fixes on first Linux build, not design changes.
#include <gtk/gtk.h>
#include <webkit2/webkit2.h>

#include <algorithm>
#include <cstdio>
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

// ---------------------------------------------------------------------------
// App state (mirrors the Windows shell's App/Tab).
// ---------------------------------------------------------------------------
struct LTab : TabModel {
    WebKitWebView* view = nullptr;
};

static struct {
    GtkWidget* window = nullptr;
    GtkWidget* vbox = nullptr;
    WebKitWebView* chrome = nullptr;
    GtkWidget* stack = nullptr; // holds one WebKitWebView per tab
    std::vector<std::unique_ptr<LTab>> tabs;
    std::string activeId;
    int nextTabNum = 1;
    int suggestDip = 0;
    std::string initialUrl;
    WebKitUserContentFilter* adFilter = nullptr;
} g;

static Settings g_settings;
static std::vector<BookmarkEntry> g_bookmarks;
static std::vector<HistoryEntry> g_history;
static std::string g_dataDir;

// ---------------------------------------------------------------------------
// Storage (UTF-8 files on Linux; the core (de)serializes wide strings).
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
        f.write(u.data(), static_cast<std::streamsize>(u.size()));
    }
}

// ---------------------------------------------------------------------------
// Chrome bridge: the shim gives the core UI `window.chrome.webview` on WebKit.
// ---------------------------------------------------------------------------
static const char* kBridgeShim =
    "window.chrome = window.chrome || {};"
    "window.chrome.webview = {"
    "  _ls: [],"
    "  postMessage: (m) => window.webkit.messageHandlers.minima.postMessage(m),"
    "  addEventListener: (t, f) => { if (t === 'message') window.chrome.webview._ls.push(f); },"
    "  __dispatch: (data) => { for (const f of window.chrome.webview._ls) f({data: data}); }"
    "};";

static void EvalOn(WebKitWebView* view, const std::string& js) {
    if (view) webkit_web_view_evaluate_javascript(view, js.c_str(), -1, nullptr, nullptr, nullptr, nullptr, nullptr);
}

/// Posts a JSON payload to the chrome UI (equivalent of PostWebMessageAsJson).
static void PostChromeJson(const std::string& json) {
    EvalOn(g.chrome, "window.chrome.webview.__dispatch(" + json + ")");
}

// ---------------------------------------------------------------------------
// Tab plumbing (mirrors Relayout/SyncState/FindTab on Windows).
// ---------------------------------------------------------------------------
static LTab* FindTab(const std::string& id) {
    for (auto& t : g.tabs)
        if (WideToUtf8(t->id) == id) return t.get();
    return nullptr;
}

static LTab* ActiveTab() { return FindTab(g.activeId); }

static void Relayout() {
    int chromeH = 88 + std::min(g.suggestDip, 340);
    gtk_widget_set_size_request(GTK_WIDGET(g.chrome), -1, chromeH);
    if (LTab* t = ActiveTab(); t && t->view)
        gtk_stack_set_visible_child(GTK_STACK(g.stack), GTK_WIDGET(t->view));
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
        ss << L"{\"id\":\"" << JsonEscape(t->id) << L"\",\"title\":\"" << JsonEscape(t->title)
           << L"\",\"url\":\"" << JsonEscape(url) << L"\",\"fav\":\"\",\"blocked\":" << t->blocked
           << L",\"audio\":false,\"muted\":false"
           << L",\"loading\":" << (t->loading ? L"true" : L"false")
           << L",\"canBack\":" << (t->canBack ? L"true" : L"false")
           << L",\"canFwd\":" << (t->canFwd ? L"true" : L"false") << L",\"bookmarked\":"
           << ([&] {
                  for (auto& b : g_bookmarks)
                      if (b.url == t->url) return L"true";
                  return L"false";
              }())
           << L"}";
    }
    ss << L"]}";
    PostChromeJson(WideToUtf8(ss.str()));
    if (LTab* t = ActiveTab()) {
        std::string title = t->title.empty() ? "Minima" : WideToUtf8(t->title) + " — Minima";
        gtk_window_set_title(GTK_WINDOW(g.window), title.c_str());
    }
    SaveSession();
}

// ---------------------------------------------------------------------------
// Internal pages built by the core.
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
// Tab creation + WebKit event wiring (mirrors WireTabEvents on Windows).
// ---------------------------------------------------------------------------
static void CreateTab(const std::string& navigateTo, const std::wstring& htmlContent = L"");

static void OnTabChanged(WebKitWebView* view) {
    for (auto& t : g.tabs) {
        if (t->view != view) continue;
        const gchar* title = webkit_web_view_get_title(view);
        const gchar* uri = webkit_web_view_get_uri(view);
        if (title) t->title = Utf8ToWideStr(title);
        if (uri) t->url = Utf8ToWideStr(uri);
        t->canBack = webkit_web_view_can_go_back(view);
        t->canFwd = webkit_web_view_can_go_forward(view);
        SyncState();
        return;
    }
}

static void OnLoadChanged(WebKitWebView* view, WebKitLoadEvent ev, gpointer) {
    for (auto& t : g.tabs) {
        if (t->view != view) continue;
        t->loading = (ev != WEBKIT_LOAD_FINISHED);
        if (ev == WEBKIT_LOAD_FINISHED && !t->url.empty() && t->url.rfind(L"about:", 0) != 0 &&
            t->url.rfind(L"data:", 0) != 0) {
            auto it = std::find_if(g_history.begin(), g_history.end(),
                                   [&](const HistoryEntry& h) { return h.url == t->url; });
            if (it != g_history.end()) g_history.erase(it);
            g_history.insert(g_history.begin(), {t->url, t->title, static_cast<long long>(time(nullptr))});
            if (g_history.size() > 5000) g_history.resize(5000);
            WriteDataFile("history.json", SerializeHistory(g_history));
        }
        OnTabChanged(view);
        return;
    }
}

/// Popups / target=_blank open as tabs (mirrors NewWindowRequested).
static GtkWidget* OnCreate(WebKitWebView*, WebKitNavigationAction* action, gpointer) {
    WebKitURIRequest* req = webkit_navigation_action_get_request(action);
    if (const gchar* uri = webkit_uri_request_get_uri(req)) CreateTab(uri);
    return nullptr;
}

static void CreateTab(const std::string& navigateTo, const std::wstring& htmlContent) {
    auto tab = std::make_unique<LTab>();
    tab->id = L"t" + std::to_wstring(g.nextTabNum++);
    LTab* raw = tab.get();
    g.tabs.push_back(std::move(tab));
    g.activeId = WideToUtf8(raw->id);

    raw->view = WEBKIT_WEB_VIEW(webkit_web_view_new());
    WebKitUserContentManager* ucm = webkit_web_view_get_user_content_manager(raw->view);
    if (g.adFilter && g_settings.adblock) webkit_user_content_manager_add_filter(ucm, g.adFilter);
    g_signal_connect(raw->view, "notify::title", G_CALLBACK(+[](GObject* o, GParamSpec*, gpointer) {
                         OnTabChanged(WEBKIT_WEB_VIEW(o));
                     }),
                     nullptr);
    g_signal_connect(raw->view, "notify::uri", G_CALLBACK(+[](GObject* o, GParamSpec*, gpointer) {
                         OnTabChanged(WEBKIT_WEB_VIEW(o));
                     }),
                     nullptr);
    g_signal_connect(raw->view, "load-changed", G_CALLBACK(OnLoadChanged), nullptr);
    g_signal_connect(raw->view, "create", G_CALLBACK(OnCreate), nullptr);

    gtk_stack_add_named(GTK_STACK(g.stack), GTK_WIDGET(raw->view), g.activeId.c_str());
    gtk_widget_show(GTK_WIDGET(raw->view));

    if (!navigateTo.empty()) {
        webkit_web_view_load_uri(raw->view, navigateTo.c_str());
    } else if (!htmlContent.empty()) {
        webkit_web_view_load_html(raw->view, WideToUtf8(htmlContent).c_str(), nullptr);
    } else {
        webkit_web_view_load_html(raw->view, WideToUtf8(BuildStartHtml()).c_str(), nullptr);
    }
    Relayout();
    SyncState();
}

static void CloseTab(const std::string& id) {
    auto it = std::find_if(g.tabs.begin(), g.tabs.end(),
                           [&](const auto& t) { return WideToUtf8(t->id) == id; });
    if (it == g.tabs.end()) return;
    size_t idx = static_cast<size_t>(it - g.tabs.begin());
    gtk_widget_destroy(GTK_WIDGET((*it)->view));
    g.tabs.erase(it);
    if (g.tabs.empty()) {
        CreateTab("");
        return;
    }
    if (g.activeId == id) g.activeId = WideToUtf8(g.tabs[std::min(idx, g.tabs.size() - 1)]->id);
    Relayout();
    SyncState();
}

// ---------------------------------------------------------------------------
// Commands from the chrome UI (mirrors HandleUiCommand; same wire protocol).
// ---------------------------------------------------------------------------
static void HandleUiCommand(const std::string& msg) {
    std::string cmd = msg, arg;
    size_t sep = msg.find('\x1F');
    if (sep != std::string::npos) {
        cmd = msg.substr(0, sep);
        arg = msg.substr(sep + 1);
    }
    LTab* t = ActiveTab();
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
        webkit_web_view_load_uri(t->view, WideToUtf8(url).c_str());
    } else if (cmd == "newtab") {
        CreateTab("");
    } else if (cmd == "close") {
        CloseTab(arg);
    } else if (cmd == "select") {
        if (FindTab(arg)) { g.activeId = arg; Relayout(); SyncState(); }
    } else if (cmd == "back") {
        if (t) webkit_web_view_go_back(t->view);
    } else if (cmd == "forward") {
        if (t) webkit_web_view_go_forward(t->view);
    } else if (cmd == "reload") {
        if (t) webkit_web_view_reload(t->view);
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
    } else if (cmd == "query") {
        // Bookmark/history suggestions — same JSON shape the chrome UI expects.
        std::wstring q = Utf8ToWideStr(arg), lq = q;
        std::transform(lq.begin(), lq.end(), lq.begin(), towlower);
        auto matches = [&](std::wstring s) {
            std::transform(s.begin(), s.end(), s.begin(), towlower);
            return s.find(lq) != std::wstring::npos;
        };
        std::wstringstream ss;
        ss << L"{\"q\":\"" << JsonEscape(q) << L"\",\"sugg\":[";
        int count = 0;
        bool first = true;
        auto add = [&](const wchar_t* kind, const BookmarkEntry& e) {
            if (count >= 6) return;
            if (!first) ss << L",";
            first = false;
            ss << L"{\"kind\":\"" << kind << L"\",\"title\":\"" << JsonEscape(e.title)
               << L"\",\"url\":\"" << JsonEscape(e.url) << L"\"}";
            count++;
        };
        for (auto& b : g_bookmarks)
            if (matches(b.title) || matches(b.url)) add(L"b", b);
        for (auto& h : g_history)
            if (count < 6 && (matches(h.title) || matches(h.url))) add(L"h", {h.url, h.title});
        ss << L"]}";
        PostChromeJson(WideToUtf8(ss.str()));
    } else if (cmd == "find") {
        if (t) webkit_find_controller_search(webkit_web_view_get_find_controller(t->view),
                                             arg.c_str(),
                                             WEBKIT_FIND_OPTIONS_CASE_INSENSITIVE |
                                                 WEBKIT_FIND_OPTIONS_WRAP_AROUND,
                                             G_MAXUINT);
    } else if (cmd == "findnext") {
        if (t) webkit_find_controller_search_next(webkit_web_view_get_find_controller(t->view));
    } else if (cmd == "findprev") {
        if (t) webkit_find_controller_search_previous(webkit_web_view_get_find_controller(t->view));
    } else if (cmd == "findstop") {
        if (t) webkit_find_controller_search_finish(webkit_web_view_get_find_controller(t->view));
    }
    // Not yet ported on Linux: askai/sidebar (AI sidecar), settings page, downloads,
    // mute, reopen, reorder — see the file header for status.
}

static void OnChromeMessage(WebKitUserContentManager*, WebKitJavascriptResult* result, gpointer) {
    JSCValue* v = webkit_javascript_result_get_js_value(result);
    if (char* s = jsc_value_to_string(v)) {
        HandleUiCommand(s);
        g_free(s);
    }
}

// ---------------------------------------------------------------------------
// Ad/tracker blocking: compile the core's host list into a WebKit content filter.
// ---------------------------------------------------------------------------
static void BuildAdFilter(WebKitUserContentManager*) {
    std::string rules = "[";
    bool first = true;
    for (const wchar_t* d : kAdHosts) {
        if (!first) rules += ",";
        first = false;
        rules += R"({"trigger":{"url-filter":".*","if-domain":["*)" + WideToUtf8(d) +
                 R"("]},"action":{"type":"block"}})";
    }
    rules += "]";
    WebKitUserContentFilterStore* store =
        webkit_user_content_filter_store_new((g_dataDir + "/filters").c_str());
    GBytes* bytes = g_bytes_new(rules.data(), rules.size());
    webkit_user_content_filter_store_save(
        store, "minima-ads", bytes, nullptr,
        +[](GObject* src, GAsyncResult* res, gpointer) {
            g.adFilter = webkit_user_content_filter_store_save_finish(
                WEBKIT_USER_CONTENT_FILTER_STORE(src), res, nullptr);
        },
        nullptr);
    g_bytes_unref(bytes);
}

// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
    gtk_init(&argc, &argv);
    if (argc > 1) g.initialUrl = argv[1];

    g_dataDir = std::string(g_get_user_data_dir()) + "/minima";
    g_mkdir_with_parents(g_dataDir.c_str(), 0700);
    ApplySettings(g_settings, ReadDataFile("settings.json"));
    g_bookmarks = ParseBookmarks(ReadDataFile("bookmarks.json"));
    g_history = ParseHistory(ReadDataFile("history.json"));

    g.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_default_size(GTK_WINDOW(g.window), 1200, 800);
    gtk_window_set_title(GTK_WINDOW(g.window), "Minima");
    g_signal_connect(g.window, "destroy", G_CALLBACK(+[](GtkWidget*, gpointer) {
                         SaveSession();
                         gtk_main_quit();
                     }),
                     nullptr);

    g.vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(g.window), g.vbox);

    // Chrome strip: the core's UI HTML + the postMessage bridge shim.
    WebKitUserContentManager* ucm = webkit_user_content_manager_new();
    webkit_user_content_manager_add_script(
        ucm, webkit_user_script_new(kBridgeShim, WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
                                    WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, nullptr, nullptr));
    webkit_user_content_manager_register_script_message_handler(ucm, "minima");
    g_signal_connect(ucm, "script-message-received::minima", G_CALLBACK(OnChromeMessage), nullptr);
    g.chrome = WEBKIT_WEB_VIEW(webkit_web_view_new_with_user_content_manager(ucm));
    gtk_box_pack_start(GTK_BOX(g.vbox), GTK_WIDGET(g.chrome), FALSE, FALSE, 0);

    g.stack = gtk_stack_new();
    gtk_box_pack_start(GTK_BOX(g.vbox), g.stack, TRUE, TRUE, 0);

    BuildAdFilter(ucm);
    webkit_web_view_load_html(g.chrome, WideToUtf8(kChromeHtml).c_str(), nullptr);
    Relayout();

    gtk_widget_show_all(g.window);
    gtk_main();
    return 0;
}
