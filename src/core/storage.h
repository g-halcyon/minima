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
           << JsonEscape(bookmarks[i].title) << L"\"}";
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
    }
}

inline std::wstring SerializeSettings(const Settings& s) {
    std::wstringstream ss;
    ss << L"{\"searchEngine\":\"" << JsonEscape(s.searchEngine) << L"\",\"aiModel\":\""
       << JsonEscape(s.aiModel) << L"\",\"adblock\":\"" << (s.adblock ? L"1" : L"0")
       << L"\",\"restoreSession\":\"" << (s.restoreSession ? L"1" : L"0") << L"\"}";
    return ss.str();
}

inline std::vector<SessionTab> ParseSession(const std::wstring& content) {
    std::vector<SessionTab> out;
    std::vector<std::vector<std::pair<std::wstring, std::wstring>>> objs;
    ParseJsonArray(content, objs);
    for (auto& o : objs) {
        SessionTab t{L"", false};
        for (auto& kv : o) {
            if (kv.first == L"url") t.url = kv.second;
            else if (kv.first == L"cur") t.current = kv.second != L"0";
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
           << (tabs[i].current ? L"1" : L"0") << L"\"}";
    }
    ss << L"]";
    return ss.str();
}

} // namespace minima
