// catalog.h — portable curated extension catalog + GitHub release-asset URL parser.
// The catalog is pure data; the parser scans a release JSON body (fetched by the shell's
// HTTP client) for the download URL of the asset we want. Downloading, unzipping and
// installing are platform work that stays in the shell. Part of the "core" per PORTING.md.
//
// Note: extensions only work on a Chromium engine (WebView2 on Windows). See PORTING.md —
// the shell hides the catalog where the platform webview can't load extensions.
#pragma once

#include <string>

namespace minima {

struct CatalogExt {
    const wchar_t* id;        // internal slug
    const wchar_t* name;      // shown name (must match the extension's own name for "installed" detection)
    const wchar_t* desc;      // one-line description
    const wchar_t* repoPath;  // GitHub API path to the latest release
    const char* assetMatch;   // substring identifying the unpacked-Chromium .zip asset
};

inline const CatalogExt kExtCatalog[] = {
    {L"ublock", L"uBlock Origin", L"Efficient, wide-spectrum ad and tracker blocker.",
     L"/repos/gorhill/uBlock/releases/latest", ".chromium.zip"},
    {L"stylus", L"Stylus", L"Restyle the web with custom themes and userstyles.",
     L"/repos/openstyles/stylus/releases/latest", "stylus-chrome-mv3"},
    {L"violentmonkey", L"Violentmonkey", L"Run userscripts that customize how sites look and behave.",
     L"/repos/violentmonkey/violentmonkey/releases/latest", "Violentmonkey-mv3"},
};

/// From a GitHub "latest release" JSON body, the first asset download URL (UTF-8)
/// whose name contains `match`, or "" if none. Pure string scan — no network.
inline std::string ParseGithubAssetUrl(const std::string& json, const std::string& match) {
    const std::string key = "\"browser_download_url\":\"";
    size_t pos = 0;
    while ((pos = json.find(key, pos)) != std::string::npos) {
        size_t start = pos + key.size();
        size_t end = json.find('"', start);
        if (end == std::string::npos) break;
        std::string url = json.substr(start, end - start);
        if (url.find(match) != std::string::npos) return url;
        pos = end;
    }
    return "";
}

} // namespace minima
