// storage.h — portable (de)serialization of Minima's data files. These turn the
// in-memory models to/from the on-disk JSON text; the actual file read/write is the
// one platform-specific piece and stays in each shell (the IStorage seam in
// platform.h). Pure std C++ (part of the "core" per PORTING.md).
#pragma once

#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

#include "json.h"
#include "models.h"

namespace minima {

inline std::vector<BookmarkEntry> ParseBookmarks(const std::wstring& content) {
    std::vector<BookmarkEntry> out;
    std::vector<std::vector<std::pair<std::wstring, std::wstring>>> objs;
    ParseJsonArray(content, objs);
    for (auto& o : objs) {
        BookmarkEntry b;
        for (auto& kv : o) {
            if (kv.first == L"url") b.url = kv.second;
            else if (kv.first == L"title") b.title = kv.second;
            else if (kv.first == L"folder") b.folder = kv.second;
        }
        if (!b.url.empty()) out.push_back(std::move(b));
    }
    return out;
}

inline std::wstring SerializeBookmarks(const std::vector<BookmarkEntry>& bookmarks) {
    std::wstringstream ss;
    ss << L"[";
    for (size_t i = 0; i < bookmarks.size(); i++) {
        if (i) ss << L",";
        ss << L"{\"url\":\"" << JsonEscape(bookmarks[i].url) << L"\",\"title\":\""
           << JsonEscape(bookmarks[i].title) << L"\"";
        if (!bookmarks[i].folder.empty()) ss << L",\"folder\":\"" << JsonEscape(bookmarks[i].folder) << L"\"";
        ss << L"}";
    }
    ss << L"]";
    return ss.str();
}

inline std::vector<HistoryEntry> ParseHistory(const std::wstring& content) {
    std::vector<HistoryEntry> out;
    std::vector<std::vector<std::pair<std::wstring, std::wstring>>> objs;
    ParseJsonArray(content, objs);
    for (auto& o : objs) {
        HistoryEntry h;
        h.time = 0;
        for (auto& kv : o) {
            if (kv.first == L"url") h.url = kv.second;
            else if (kv.first == L"title") h.title = kv.second;
            else if (kv.first == L"time") h.time = wcstoll(kv.second.c_str(), nullptr, 10);
        }
        if (!h.url.empty()) out.push_back(std::move(h));
    }
    return out;
}

inline std::wstring SerializeHistory(const std::vector<HistoryEntry>& history) {
    std::wstringstream ss;
    ss << L"[";
    for (size_t i = 0; i < history.size(); i++) {
        if (i) ss << L",";
        ss << L"{\"url\":\"" << JsonEscape(history[i].url) << L"\",\"title\":\""
           << JsonEscape(history[i].title) << L"\",\"time\":" << history[i].time << L"}";
    }
    ss << L"]";
    return ss.str();
}

inline void ApplySettings(Settings& s, const std::wstring& content) {
    std::vector<std::pair<std::wstring, std::wstring>> obj;
    size_t i = 0;
    ParseJsonObject(content, i, obj);
    for (auto& kv : obj) {
        if (kv.first == L"searchEngine") s.searchEngine = kv.second;
        else if (kv.first == L"aiModel") s.aiModel = kv.second;
        else if (kv.first == L"adblock") s.adblock = kv.second != L"0";
        else if (kv.first == L"restoreSession") s.restoreSession = kv.second != L"0";
        else if (kv.first == L"aiEnabled") s.aiEnabled = kv.second != L"0";
        else if (kv.first == L"aiIdleUnload") s.aiIdleUnload = kv.second != L"0";
        else if (kv.first == L"sleepTabs") s.sleepTabs = kv.second != L"0";
        else if (kv.first == L"aiGpu") s.aiGpu = kv.second != L"0";
        else if (kv.first == L"savePasswords") s.savePasswords = kv.second != L"0";
        else if (kv.first == L"memory") s.memory = kv.second != L"0";
        else if (kv.first == L"memoryAsked") s.memoryAsked = kv.second != L"0";
        else if (kv.first == L"memoryExclude") s.memoryExclude = kv.second;
        else if (kv.first == L"readerSize") {
            int v = static_cast<int>(wcstol(kv.second.c_str(), nullptr, 10));
            if (v >= 14 && v <= 30) s.readerSize = v;
        } else if (kv.first == L"readerFont") {
            if (kv.second == L"serif" || kv.second == L"sans") s.readerFont = kv.second;
        } else if (kv.first == L"readerTheme") {
            if (kv.second == L"auto" || kv.second == L"light" || kv.second == L"sepia" || kv.second == L"dark")
                s.readerTheme = kv.second;
        }
    }
}

/// Reader-view preferences posted by the reader overlay: {"size":19,"font":"serif","theme":"auto"}.
/// Unknown keys and out-of-range values are ignored (the message can come from any page).
inline void ApplyReaderPrefs(Settings& s, const std::wstring& json) {
    std::vector<std::pair<std::wstring, std::wstring>> obj;
    size_t i = 0;
    ParseJsonObject(json, i, obj);
    std::wstring mapped = L"{";
    for (auto& kv : obj) {
        const wchar_t* key = kv.first == L"size" ? L"readerSize" : kv.first == L"font" ? L"readerFont"
                           : kv.first == L"theme" ? L"readerTheme" : nullptr;
        if (!key) continue;
        if (mapped.size() > 1) mapped += L",";
        mapped += std::wstring(L"\"") + key + L"\":\"" + JsonEscape(kv.second) + L"\"";
    }
    ApplySettings(s, mapped + L"}"); // reuses ApplySettings' validation of the reader fields
}

inline std::wstring SerializeSettings(const Settings& s) {
    auto flag = [](bool b) { return b ? L"1" : L"0"; };
    std::wstringstream ss;
    ss << L"{\"searchEngine\":\"" << JsonEscape(s.searchEngine) << L"\",\"aiModel\":\""
       << JsonEscape(s.aiModel) << L"\",\"adblock\":\"" << flag(s.adblock)
       << L"\",\"restoreSession\":\"" << flag(s.restoreSession) << L"\",\"aiEnabled\":\""
       << flag(s.aiEnabled) << L"\",\"aiIdleUnload\":\"" << flag(s.aiIdleUnload)
       << L"\",\"sleepTabs\":\"" << flag(s.sleepTabs) << L"\",\"aiGpu\":\"" << flag(s.aiGpu)
       << L"\",\"savePasswords\":\"" << flag(s.savePasswords) << L"\",\"memory\":\"" << flag(s.memory)
       << L"\",\"memoryAsked\":\"" << flag(s.memoryAsked) << L"\",\"memoryExclude\":\""
       << JsonEscape(s.memoryExclude) << L"\",\"readerSize\":\"" << s.readerSize << L"\",\"readerFont\":\""
       << JsonEscape(s.readerFont) << L"\",\"readerTheme\":\"" << JsonEscape(s.readerTheme) << L"\"}";
    return ss.str();
}

inline std::vector<SessionTab> ParseSession(const std::wstring& content) {
    std::vector<SessionTab> out;
    std::vector<std::vector<std::pair<std::wstring, std::wstring>>> objs;
    ParseJsonArray(content, objs);
    for (auto& o : objs) {
        SessionTab t{L"", false, L""};
        for (auto& kv : o) {
            if (kv.first == L"url") t.url = kv.second;
            else if (kv.first == L"cur") t.current = kv.second != L"0";
            else if (kv.first == L"title") t.title = kv.second;
        }
        if (!t.url.empty()) out.push_back(std::move(t));
    }
    return out;
}

inline std::wstring SerializeSession(const std::vector<SessionTab>& tabs) {
    std::wstringstream ss;
    ss << L"[";
    for (size_t i = 0; i < tabs.size(); i++) {
        if (i) ss << L",";
        ss << L"{\"url\":\"" << JsonEscape(tabs[i].url) << L"\",\"cur\":\""
           << (tabs[i].current ? L"1" : L"0") << L"\",\"title\":\"" << JsonEscape(tabs[i].title)
           << L"\"}";
    }
    ss << L"]";
    return ss.str();
}

} // namespace minima
