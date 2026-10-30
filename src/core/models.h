// models.h — portable data models and the search-engine table. Pure std C++, no
// platform dependencies (part of the "core" per PORTING.md). The live Tab object stays
// in the shell because it owns a native webview handle; everything here is plain data.
#pragma once

#include <string>

namespace minima {

// `folder` is empty for bookmarks made in Minima; imported ones keep their source folder
// ("Bookmarks bar", "Other bookmarks/Recipes", …) so a big import stays organized.
struct BookmarkEntry { std::wstring url, title, folder; };
struct HistoryEntry { std::wstring url, title; long long time; };

// The portable state of a browser tab — everything except the native webview handle,
// which each shell adds by deriving its own Tab from this (see src/main.cpp).
struct TabModel {
    std::wstring id;
    std::wstring title = L"New Tab";
    std::wstring url;
    std::wstring favicon;
    int blocked = 0;      // ad/tracker requests blocked on the current page
    double zoom = 1.0;
    bool audio = false;   // page is currently playing audio
    bool muted = false;
    bool loading = false;
    bool canBack = false;
    bool canFwd = false;
    bool isAiPage = false;
    bool isSettingsPage = false;
    bool isWizardPage = false;
    bool isHistoryPage = false; // History & Memory search page
    bool isStartPage = false;   // the new-tab page (until the tab navigates elsewhere)
    bool readerable = false;    // the page has an article reader view can show
};

struct Settings {
    std::wstring searchEngine = L"mojeek";
    std::wstring aiModel = L"gemma-3-1b";
    bool adblock = true;
    bool restoreSession = true; // reopen last session's tabs on startup
    bool aiEnabled = true;      // master switch for every on-device AI surface
    bool aiIdleUnload = true;   // free the model's memory after a while unused
    bool sleepTabs = true;      // suspend background tabs that haven't been used in a while
    bool aiGpu = true;          // let the AI engine use the GPU (Vulkan) when available
    bool savePasswords = true;  // WebView2's built-in password saving/filling
    bool memory = false;        // remember the text of pages read, for search (off until chosen)
    bool memoryAsked = false;   // the user has seen the Memory opt-in (wizard or start page)
    std::wstring memoryExclude; // newline-separated hosts never remembered
    int readerSize = 19;        // reader view: font size (px), font ("serif"/"sans"), theme
    std::wstring readerFont = L"serif";
    std::wstring readerTheme = L"auto";
};

// One tab in a saved session; `current` marks the tab that was active. The title lets a
// restored tab show its name before it has loaded (tabs are restored lazily).
struct SessionTab { std::wstring url; bool current; std::wstring title; };

struct SearchEngine { const wchar_t* id; const wchar_t* label; const wchar_t* urlPrefix; };

inline const SearchEngine kSearchEngines[] = {
    {L"mojeek", L"Mojeek (private, no tracking)", L"https://www.mojeek.com/search?q="},
    {L"duckduckgo", L"DuckDuckGo", L"https://duckduckgo.com/?q="},
    {L"google", L"Google", L"https://www.google.com/search?q="},
    {L"bing", L"Bing", L"https://www.bing.com/search?q="},
    {L"brave", L"Brave Search", L"https://search.brave.com/search?q="},
    {L"startpage", L"Startpage", L"https://www.startpage.com/sp/search?query="},
};

/// The engine with the given id, or the first (default) if unknown.
inline const SearchEngine& FindSearchEngine(const std::wstring& id) {
    for (auto& e : kSearchEngines)
        if (id == e.id) return e;
    return kSearchEngines[0];
}

} // namespace minima
