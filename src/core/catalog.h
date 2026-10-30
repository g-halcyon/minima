// catalog.h — portable curated extension catalog + GitHub release-asset URL parser.
// The catalog is pure data; the parser scans a release JSON body (fetched by the shell's
// HTTP client) for the download URL of the asset we want. Downloading, unzipping and
// installing are platform work that stays in the shell. Part of the "core" per PORTING.md.
//
// Note: extensions only work on a Chromium engine (WebView2 on Windows). See PORTING.md —
// the shell hides the catalog where the platform webview can't load extensions.
#pragma once

#include <string>
#include <vector>

namespace minima {

struct CatalogExt {
    const wchar_t* id;        // internal slug
    const wchar_t* name;      // shown name (must match the extension's own name for "installed" detection)
    const wchar_t* desc;      // one-line description
    const wchar_t* repo;      // GitHub "owner/name"; the install uses its latest release
    const char* assetMatch;   // substring identifying the unpacked-Chromium .zip asset
};

// uBlock Origin Lite rather than classic uBlock Origin: the classic build is Manifest V2,
// which Chromium (and so WebView2) is retiring. Lite is the same team's Manifest V3 edition.
inline const CatalogExt kExtCatalog[] = {
    {L"ublocklite", L"uBlock Origin Lite", L"Efficient ad and tracker blocker (Manifest V3 edition).",
     L"uBlockOrigin/uBOL-home", ".chromium.zip"},
    {L"stylus", L"Stylus", L"Restyle the web with custom themes and userstyles.",
     L"openstyles/stylus", "stylus-chrome-mv3"},
    {L"violentmonkey", L"Violentmonkey", L"Run userscripts that customize how sites look and behave.",
     L"violentmonkey/violentmonkey", "Violentmonkey-mv3"},
};

// ---------------------------------------------------------------------------
// Fallback for when the GitHub API refuses us (it allows 60 unauthenticated requests per
// hour per IP, which shared/NAT'd networks exhaust): the same data is on the ordinary
// release web pages. A release page lazy-loads its downloads from
// /<owner>/<repo>/releases/expanded_assets/<tag>, whose links are
// /<owner>/<repo>/releases/download/<tag>/<file>.
// ---------------------------------------------------------------------------

/// Release tags mentioned in a GitHub releases HTML page, without duplicates. Tags from the
/// page's expanded_assets include (one per release shown — authoritative) come first, then
/// /releases/tag/ links in page order (newest first on the list page). The page also holds
/// JS templates ("/releases/tag/*name"), escaped JSON and links to older releases in the
/// notes, so only real tag characters are accepted.
inline std::vector<std::string> ParseGithubTags(const std::string& html) {
    auto tagChar = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
               c == '-' || c == '_' || c == '+';
    };
    std::vector<std::string> tags;
    for (const std::string key : {"/releases/expanded_assets/", "/releases/tag/"}) {
        size_t pos = 0;
        while ((pos = html.find(key, pos)) != std::string::npos) {
            size_t start = pos + key.size(), end = start;
            while (end < html.size() && tagChar(html[end])) end++;
            std::string tag = html.substr(start, end - start);
            bool seen = false;
            for (auto& t : tags) seen = seen || t == tag;
            if (!tag.empty() && !seen) tags.push_back(tag);
            pos = end;
        }
    }
    return tags;
}

/// From an expanded_assets HTML fragment, the absolute URL of the first download whose
/// file path contains `match`, or "".
inline std::string ParseGithubDownloadHref(const std::string& html, const std::string& match) {
    const std::string key = "href=\"";
    size_t pos = 0;
    while ((pos = html.find(key, pos)) != std::string::npos) {
        size_t start = pos + key.size();
        size_t end = html.find('"', start);
        if (end == std::string::npos) break;
        std::string href = html.substr(start, end - start);
        if (href.find("/releases/download/") != std::string::npos && href.find(match) != std::string::npos)
            return href.rfind("https://", 0) == 0 ? href : "https://github.com" + href;
        pos = end;
    }
    return "";
}

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
