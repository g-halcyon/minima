// selftest.cpp — proves the core is genuinely platform-independent: it includes every
// core header WITHOUT windows.h / WebView2 / any platform API and exercises the logic.
// If this compiles and runs on a toolchain, the core is portable to that toolchain.
//
// Not part of the app build. Build & run standalone, e.g. on Windows:
//   cl /nologo /std:c++17 /EHsc /utf-8 src\core\selftest.cpp /Fe:build\selftest.exe
//   build\selftest.exe
// or on any platform with a C++17 compiler:
//   c++ -std=c++17 src/core/selftest.cpp -o build/selftest && ./build/selftest
#include <cassert>
#include <cwchar>
#include <cstdio>

#include "adblock.h"
#include "ai_models.h"
#include "catalog.h"
#include "json.h"
#include "models.h"
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

    // GitHub asset-URL parser.
    std::string json = R"([{"browser_download_url":"https://h/uBlock0_1.2.chromium.zip"}])";
    check("github asset parse", ParseGithubAssetUrl(json, ".chromium.zip").find("uBlock0") != std::string::npos);

    // Tables + UI assets are linked in (non-empty).
    check("catalog present", kExtCatalog[0].name != nullptr);
    check("ui assets present", kChromeHtml[0] == L'<');

    std::printf(failures ? "\nCORE SELFTEST FAILED (%d)\n" : "\nCORE SELFTEST OK\n", failures);
    return failures ? 1 : 0;
}
