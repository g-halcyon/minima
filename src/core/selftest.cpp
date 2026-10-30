// selftest.cpp — proves the core is genuinely platform-independent: it includes every
// core header WITHOUT windows.h / WebView2 / any platform API and exercises the logic.
// If this compiles and runs on a toolchain, the core is portable to that toolchain.
//
// Not part of the app build. Build & run standalone (needs the SQLite object build.bat
// makes, or compile vendor/sqlite/sqlite3.c alongside with -DSQLITE_ENABLE_FTS5), e.g.:
//   cl /nologo /std:c++17 /EHsc /utf-8 /I vendor\sqlite src\core\selftest.cpp build\sqlite3.obj /Fe:build\selftest.exe
//   build\selftest.exe
// or on any platform with a C++17 compiler:
//   cc -c -DSQLITE_ENABLE_FTS5 vendor/sqlite/sqlite3.c -o build/sqlite3.o
//   c++ -std=c++17 -Ivendor/sqlite src/core/selftest.cpp build/sqlite3.o -o build/selftest && ./build/selftest
#include <cassert>
#include <cwchar>
#include <cstdio>

#include "adblock.h"
#include "ai_models.h"
#include "catalog.h"
#include "importer.h"
#include "json.h"
#include "memory.h"
#include "models.h"
#include "reader.h"
#include "storage.h"
#include "ui_assets.h"
#include "urls.h"

using namespace minima;

int main() {
    int failures = 0;
    auto check = [&](const char* name, bool ok) {
        std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", name);
        if (!ok) failures++;
    };

    // URL vs. search resolution.
    check("url passthrough", ResolveInputToUrl(L"https://a.com", L"S?q=") == L"https://a.com");
    check("bare domain -> https", ResolveInputToUrl(L"example.com", L"S?q=") == L"https://example.com");
    check("query -> search+encode", ResolveInputToUrl(L"hello world", L"S?q=") == L"S?q=hello+world");

    // Search-engine + AI-model lookup.
    check("search engine lookup", FindSearchEngine(L"google").id == std::wstring(L"google"));
    check("search engine default", FindSearchEngine(L"nope").id == std::wstring(L"mojeek"));
    check("ai model lookup", FindAiModel(L"gemma-3-4b").id == std::wstring(L"gemma-3-4b"));

    // Ad-host matching (case-insensitive, subdomain-aware).
    check("adhost exact", IsAdHost(L"doubleclick.net"));
    check("adhost subdomain", IsAdHost(L"ads.DOUBLECLICK.net"));
    check("adhost miss", !IsAdHost(L"example.com"));

    // Storage round-trips.
    std::vector<BookmarkEntry> bm = {{L"https://x.com", L"X \"quote\""}, {L"https://y.com", L"Y"}};
    check("bookmark round-trip", ParseBookmarks(SerializeBookmarks(bm)).size() == 2 &&
                                 ParseBookmarks(SerializeBookmarks(bm))[0].title == L"X \"quote\"");
    Settings s; s.searchEngine = L"brave"; s.adblock = false;
    Settings s2; ApplySettings(s2, SerializeSettings(s));
    check("settings round-trip", s2.searchEngine == L"brave" && s2.adblock == false);
    Settings s3; s3.aiEnabled = false; s3.aiIdleUnload = false; s3.sleepTabs = false;
    Settings s4; ApplySettings(s4, SerializeSettings(s3));
    check("settings ai/sleep flags round-trip", !s4.aiEnabled && !s4.aiIdleUnload && !s4.sleepTabs);
    Settings s5; ApplySettings(s5, L"{\"searchEngine\":\"bing\",\"adblock\":\"1\"}"); // pre-upgrade file
    check("settings new flags default on", s5.aiEnabled && s5.aiIdleUnload && s5.sleepTabs);
    std::vector<SessionTab> sess = {{L"https://a.com", true, L"A \"t\""}, {L"https://b.com", false}};
    auto sess2 = ParseSession(SerializeSession(sess));
    check("session round-trip with titles", sess2.size() == 2 && sess2[0].current && sess2[0].title == L"A \"t\"" &&
                                             sess2[1].title.empty());
    check("session without titles (old file)", ParseSession(L"[{\"url\":\"https://x\",\"cur\":\"1\"}]").size() == 1);

    // GitHub asset-URL parser.
    std::string json = R"([{"browser_download_url":"https://h/uBlock0_1.2.chromium.zip"}])";
    check("github asset parse", ParseGithubAssetUrl(json, ".chromium.zip").find("uBlock0") != std::string::npos);

    // GitHub web-page fallback parsers (shapes taken from the live pages).
    std::string latestPage = R"(<script>u="/ggml-org/llama.cpp/releases/tag/*name"</script>
      {"url":"/x/releases/tag/b11960\&quot;"} <a href="/ggml-org/llama.cpp/releases/tag/v0.8.0">v0.8.0</a>
      <include-fragment src="https://github.com/ggml-org/llama.cpp/releases/expanded_assets/v0.8.0">)";
    std::string listPage = R"(<a href="/ggml-org/llama.cpp/releases/tag/v0.8.0">x</a>
      <a href="/ggml-org/llama.cpp/releases/tag/b12015">y</a><a href="/ggml-org/llama.cpp/releases/tag/b12015">dup</a>
      <a href="/ggml-org/llama.cpp/releases/tag/b12014">z</a>)";
    auto tags = ParseGithubTags(latestPage);
    check("github tags: expanded_assets first, junk skipped",
          tags.size() == 2 && tags[0] == "v0.8.0" && tags[1] == "b11960");
    auto tags2 = ParseGithubTags(listPage);
    check("github tags in order, deduped", tags2.size() == 3 && tags2[1] == "b12015" && tags2[2] == "b12014");
    std::string assets = R"(<a href="/ggml-org/llama.cpp/releases/download/b12015/llama-b12015-bin-win-cpu-arm64.zip">
      <a href="/ggml-org/llama.cpp/releases/download/b12015/llama-b12015-bin-win-cpu-x64.zip">
      <a href="/ggml-org/llama.cpp/archive/refs/tags/b12015.zip">)";
    check("github download href", ParseGithubDownloadHref(assets, "-bin-win-cpu-x64.zip") ==
                                      "https://github.com/ggml-org/llama.cpp/releases/download/b12015/llama-b12015-bin-win-cpu-x64.zip");
    check("github download href miss", ParseGithubDownloadHref(assets, ".chromium.zip").empty());

    // Memory: FTS query building can't be used for injection; stemming/prefix/snippets work.
    check("fts query terms", BuildFtsQuery(L"Rust allocator!") == L"(\"rust\" OR \"rust\"*) AND (\"allocator\" OR \"allocator\"*)");
    check("fts query strips operators", BuildFtsQuery(L"zz\" OR NEAR(x) *") == L"\"zz\" AND (\"near\" OR \"near\"*)");
    check("fts query drops filler, keeps short words exact", BuildFtsQuery(L"sign in to github") == L"(\"sign\" OR \"sign\"*) AND (\"github\" OR \"github\"*)");
    check("fts query all-stopwords kept", BuildFtsQuery(L"to be") == L"\"to\" AND \"be\"");
    check("fts query empty", BuildFtsQuery(L"!!").empty());
    {
        MemoryStore m;
        check("memory open", m.Open(":memory:"));
        m.Add(L"https://a.example/bloom", L"Bloom filters explained", L"a.example",
              L"A Bloom filter is a space-efficient probabilistic data structure. False positives are possible.", 100, 15);
        m.Add(L"https://blog.b.example/rust", L"Running Rust on microcontrollers", L"b.example",
              L"Embedded developers are running Rust with no allocator at all.", 200, 10);
        auto h1 = m.Search(L"probabilistic", 5);
        check("memory search finds body text", h1.size() == 1 && h1[0].url == L"https://a.example/bloom");
        check("memory snippet marks match", !h1.empty() && h1[0].snippet.find(L'\x02') != std::wstring::npos);
        check("memory stemming (runs -> running)", m.Search(L"runs", 5).size() == 1);
        check("memory stemmed whole word (developer -> developers)", m.Search(L"developer allocators", 5).size() == 1);
        check("memory prefix as you type", m.Search(L"micro", 5).size() == 1);
        check("memory multi-word match (catches FTS syntax errors)", m.Search(L"bloom probabilistic", 5).size() == 1);
        check("memory AND semantics", m.Search(L"bloom rust", 5).empty());
        m.Add(L"https://a.example/bloom", L"Bloom filters explained", L"a.example", L"Now about cuckoo filters.", 300, 4);
        check("memory revisit replaces text", m.Search(L"probabilistic", 5).empty() && m.Search(L"cuckoo", 5).size() == 1 &&
                                                  m.Count() == 2);
        m.ForgetSite(L"b.example"); // subdomain blog.b.example goes too
        check("memory forget site incl. subdomains", m.Count() == 1 && m.Search(L"rust", 5).empty());
        m.Add(L"https://c.example/1", L"one", L"c", L"alpha", 50, 1);
        m.Prune(1);
        check("memory prune keeps newest", m.Count() == 1 && m.Search(L"cuckoo", 5).size() == 1);
        m.Forget(L"https://a.example/bloom");
        check("memory forget page", m.Count() == 0);
    }

    // Import: Chromium bookmarks JSON (nested folders, bar first, escapes) + Firefox ini.
    std::string chromium = R"J({"roots":{
      "other":{"type":"folder","name":"Other bookmarks","children":[
        {"type":"folder","name":"Recipes","children":[{"type":"url","name":"Pie \u00e9 \ud83c\udf70","url":"https://pie.example/"}]},
        {"type":"url","name":"js","url":"javascript:alert(1)"}]},
      "bookmark_bar":{"type":"folder","name":"Bookmarks bar","children":[{"type":"url","name":"Bar","url":"https://bar.example/"}]}}})J";
    auto cb = ParseChromiumBookmarks(chromium);
    check("chromium bookmarks parsed, bar first, non-web skipped",
          cb.size() == 2 && cb[0].url == L"https://bar.example/" && cb[0].folder == L"Bookmarks bar" &&
              cb[1].folder == L"Other bookmarks/Recipes");
    check("chromium unicode escapes incl. surrogates", cb.size() == 2 && cb[1].title == L"Pie \u00e9 \U0001F370");
    bool rel = false;
    std::string ini = "[Profile1]\r\nName=default\r\nIsRelative=1\r\nPath=Profiles/old.default\r\nDefault=1\r\n\r\n"
                      "[Install308046B0AF4A39CB]\r\nDefault=Profiles/abc.default-release\r\nLocked=1\r\n";
    check("firefox default profile (install section wins)",
          ParseFirefoxDefaultProfile(ini, rel) == "Profiles/abc.default-release" && rel);
    std::string ini2 = "[Profile0]\nName=x\nIsRelative=0\nPath=D:\\FF\\p\nDefault=1\n";
    check("firefox default profile (absolute)", ParseFirefoxDefaultProfile(ini2, rel) == "D:\\FF\\p" && !rel);
    std::vector<BookmarkEntry> mine = {{L"https://bar.example/", L"Mine", L""}};
    check("merge bookmarks dedupes", MergeBookmarks(mine, cb) == 1 && mine.size() == 2 && mine[0].title == L"Mine");
    std::vector<HistoryEntry> hist = {{L"https://x/", L"X", 10}};
    check("merge history newest first + dedupe",
          MergeHistory(hist, {{L"https://y/", L"Y", 30}, {L"https://x/", L"X", 20}}, 10) == 1 && hist.size() == 2 &&
              hist[0].url == L"https://y/" && hist[1].time == 20);

    // Tables + UI assets are linked in (non-empty).
    check("catalog present", kExtCatalog[0].name != nullptr);
    check("ui assets present", kChromeHtml[0] == L'<');
    check("reader scripts present", kExtractJs[0] == L'(' && kReaderJs[0] == L'(');

    std::printf(failures ? "\nCORE SELFTEST FAILED (%d)\n" : "\nCORE SELFTEST OK\n", failures);
    return failures ? 1 : 0;
}
