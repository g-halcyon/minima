// importer.h — bring bookmarks and history over from another browser, so trying Minima
// doesn't mean starting from zero. Chromium-family browsers (Edge, Chrome, Brave, Vivaldi,
// Opera) keep bookmarks in a JSON file and history in SQLite; Firefox keeps both in
// places.sqlite. Passwords are deliberately not imported: Chromium encrypts them with
// OS-/app-bound keys, and copying secrets between apps is not something Minima should do.
// Part of the portable "core" per PORTING.md — the shell finds the files and copies the
// (locked) databases somewhere readable first.
#pragma once

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "models.h"
#include "sqlite3.h"
#include "utf.h"

namespace minima {

// ---------------------------------------------------------------------------
// Minimal JSON tree (UTF-8), enough for Chromium's nested Bookmarks file.
// ---------------------------------------------------------------------------
struct JNode {
    enum Type { Null, Bool, Num, Str, Arr, Obj } type = Null;
    std::string str;
    double num = 0;
    bool boolean = false;
    std::vector<JNode> arr;
    std::vector<std::pair<std::string, JNode>> obj;

    const JNode* get(const char* key) const {
        for (auto& kv : obj)
            if (kv.first == key) return &kv.second;
        return nullptr;
    }
    std::string text(const char* key) const {
        const JNode* n = get(key);
        return n && n->type == Str ? n->str : "";
    }
};

class JParser {
public:
    explicit JParser(const std::string& s) : s_(s) {}
    bool Parse(JNode& out) {
        Ws();
        return Value(out, 0);
    }

private:
    const std::string& s_;
    size_t i_ = 0;

    void Ws() { while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\n' || s_[i_] == '\r' || s_[i_] == '\t')) i_++; }
    static void AppendUtf8(std::string& out, unsigned cp) {
        if (cp < 0x80) out += static_cast<char>(cp);
        else if (cp < 0x800) { out += static_cast<char>(0xC0 | (cp >> 6)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
    bool Hex4(unsigned& v) {
        if (i_ + 4 > s_.size()) return false;
        v = static_cast<unsigned>(strtoul(s_.substr(i_, 4).c_str(), nullptr, 16));
        i_ += 4;
        return true;
    }
    bool String(std::string& out) {
        if (s_[i_] != '"') return false;
        i_++;
        while (i_ < s_.size() && s_[i_] != '"') {
            char c = s_[i_++];
            if (c != '\\') { out += c; continue; }
            if (i_ >= s_.size()) return false;
            char e = s_[i_++];
            switch (e) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'u': {
                    unsigned cp = 0, lo = 0;
                    if (!Hex4(cp)) return false;
                    if (cp >= 0xD800 && cp < 0xDC00 && i_ + 1 < s_.size() && s_[i_] == '\\' && s_[i_ + 1] == 'u') {
                        i_ += 2;
                        if (!Hex4(lo)) return false;
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    AppendUtf8(out, cp);
                    break;
                }
                default: out += e;
            }
        }
        if (i_ >= s_.size()) return false;
        i_++;
        return true;
    }
    bool Value(JNode& n, int depth) {
        if (depth > 64 || i_ >= s_.size()) return false;
        char c = s_[i_];
        if (c == '"') { n.type = JNode::Str; return String(n.str); }
        if (c == '{') {
            n.type = JNode::Obj;
            i_++;
            Ws();
            if (i_ < s_.size() && s_[i_] == '}') { i_++; return true; }
            while (i_ < s_.size()) {
                Ws();
                std::string key;
                if (!String(key)) return false;
                Ws();
                if (i_ >= s_.size() || s_[i_] != ':') return false;
                i_++;
                Ws();
                n.obj.emplace_back(std::move(key), JNode{});
                if (!Value(n.obj.back().second, depth + 1)) return false;
                Ws();
                if (i_ < s_.size() && s_[i_] == ',') { i_++; continue; }
                if (i_ < s_.size() && s_[i_] == '}') { i_++; return true; }
                return false;
            }
            return false;
        }
        if (c == '[') {
            n.type = JNode::Arr;
            i_++;
            Ws();
            if (i_ < s_.size() && s_[i_] == ']') { i_++; return true; }
            while (i_ < s_.size()) {
                Ws();
                n.arr.emplace_back();
                if (!Value(n.arr.back(), depth + 1)) return false;
                Ws();
                if (i_ < s_.size() && s_[i_] == ',') { i_++; continue; }
                if (i_ < s_.size() && s_[i_] == ']') { i_++; return true; }
                return false;
            }
            return false;
        }
        if (s_.compare(i_, 4, "true") == 0) { n.type = JNode::Bool; n.boolean = true; i_ += 4; return true; }
        if (s_.compare(i_, 5, "false") == 0) { n.type = JNode::Bool; i_ += 5; return true; }
        if (s_.compare(i_, 4, "null") == 0) { i_ += 4; return true; }
        size_t start = i_;
        while (i_ < s_.size() && (isdigit(static_cast<unsigned char>(s_[i_])) || strchr("+-.eE", s_[i_]))) i_++;
        if (i_ == start) return false;
        n.type = JNode::Num;
        n.num = strtod(s_.substr(start, i_ - start).c_str(), nullptr);
        return true;
    }
};

inline bool IsWebUrl(const std::string& u) { return u.rfind("http://", 0) == 0 || u.rfind("https://", 0) == 0; }

// ---------------------------------------------------------------------------
// Chromium family: <profile>/Bookmarks (JSON) and <profile>/History (SQLite).
// ---------------------------------------------------------------------------
inline void CollectChromiumNode(const JNode& n, const std::string& folder, std::vector<BookmarkEntry>& out) {
    std::string type = n.text("type");
    if (type == "url") {
        std::string url = n.text("url");
        if (IsWebUrl(url)) out.push_back({Utf8ToWideStr(url), Utf8ToWideStr(n.text("name")), Utf8ToWideStr(folder)});
        return;
    }
    if (const JNode* kids = n.get("children")) {
        std::string sub = folder;
        if (type == "folder" && !n.text("name").empty() && !folder.empty()) sub = folder + "/" + n.text("name");
        for (auto& k : kids->arr) CollectChromiumNode(k, sub, out);
    }
}

/// Bookmarks-bar items first (they're the ones people actually use), then the rest.
inline std::vector<BookmarkEntry> ParseChromiumBookmarks(const std::string& json) {
    std::vector<BookmarkEntry> out;
    JNode root;
    if (!JParser(json).Parse(root)) return out;
    const JNode* roots = root.get("roots");
    if (!roots) return out;
    const std::pair<const char*, const char*> order[] = {
        {"bookmark_bar", "Bookmarks bar"}, {"other", "Other bookmarks"}, {"synced", "Mobile bookmarks"}};
    for (auto& r : order)
        if (const JNode* n = roots->get(r.first))
            if (const JNode* kids = n->get("children")) // the root itself is named by `order`
                for (auto& k : kids->arr) CollectChromiumNode(k, r.second, out);
    return out;
}

inline std::wstring SqlText(sqlite3_stmt* st, int col) {
    const unsigned char* p = sqlite3_column_text(st, col);
    return Utf8ToWideStr(p ? reinterpret_cast<const char*>(p) : "");
}

/// Most recent `limit` visited pages. Chromium times are µs since 1601-01-01 (UTC).
inline std::vector<HistoryEntry> ReadChromiumHistory(sqlite3* db, int limit) {
    std::vector<HistoryEntry> out;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db,
                           "SELECT url, title, last_visit_time FROM urls WHERE hidden = 0 AND url LIKE 'http%'"
                           " ORDER BY last_visit_time DESC LIMIT ?",
                           -1, &st, nullptr) != SQLITE_OK)
        return out;
    sqlite3_bind_int(st, 1, limit);
    while (sqlite3_step(st) == SQLITE_ROW) {
        long long t = sqlite3_column_int64(st, 2) / 1000000 - 11644473600LL;
        out.push_back({SqlText(st, 0), SqlText(st, 1), t > 0 ? t : 0});
    }
    sqlite3_finalize(st);
    return out;
}

// ---------------------------------------------------------------------------
// Firefox: <profile>/places.sqlite holds bookmarks and history.
// ---------------------------------------------------------------------------
inline std::vector<BookmarkEntry> ReadFirefoxBookmarks(sqlite3* db) {
    struct Node { long long parent; std::string title, guid; };
    std::map<long long, Node> folders;
    struct Mark { long long parent; std::string url, title; };
    std::vector<Mark> marks;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db,
                           "SELECT b.id, b.type, b.parent, b.title, b.guid, p.url FROM moz_bookmarks b"
                           " LEFT JOIN moz_places p ON p.id = b.fk ORDER BY b.parent, b.position",
                           -1, &st, nullptr) != SQLITE_OK)
        return {};
    while (sqlite3_step(st) == SQLITE_ROW) {
        long long id = sqlite3_column_int64(st, 0), parent = sqlite3_column_int64(st, 2);
        int type = sqlite3_column_int(st, 1);
        auto txt = [&](int c) {
            const unsigned char* p = sqlite3_column_text(st, c);
            return std::string(p ? reinterpret_cast<const char*>(p) : "");
        };
        if (type == 2) folders[id] = {parent, txt(3), txt(4)};
        else if (type == 1 && IsWebUrl(txt(5))) marks.push_back({parent, txt(5), txt(3)});
    }
    sqlite3_finalize(st);
    auto rootName = [](const std::string& guid) -> std::string {
        if (guid == "toolbar_____") return "Bookmarks bar";
        if (guid == "menu________") return "Bookmarks menu";
        if (guid == "unfiled_____") return "Other bookmarks";
        if (guid == "mobile______") return "Mobile bookmarks";
        return "";
    };
    auto folderPath = [&](long long id) {
        std::string path;
        for (int depth = 0; depth < 32; depth++) {
            auto it = folders.find(id);
            if (it == folders.end() || it->second.guid == "root________") break;
            std::string name = rootName(it->second.guid);
            if (!name.empty()) return path.empty() ? name : name + "/" + path;
            path = path.empty() ? it->second.title : it->second.title + "/" + path;
            id = it->second.parent;
        }
        return path;
    };
    std::vector<BookmarkEntry> bar, rest;
    for (auto& m : marks) {
        std::string folder = folderPath(m.parent);
        BookmarkEntry b{Utf8ToWideStr(m.url), Utf8ToWideStr(m.title), Utf8ToWideStr(folder)};
        (folder.rfind("Bookmarks bar", 0) == 0 ? bar : rest).push_back(std::move(b));
    }
    bar.insert(bar.end(), rest.begin(), rest.end());
    return bar;
}

/// Most recent `limit` visited pages. Firefox times are µs since 1970.
inline std::vector<HistoryEntry> ReadFirefoxHistory(sqlite3* db, int limit) {
    std::vector<HistoryEntry> out;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db,
                           "SELECT url, title, last_visit_date FROM moz_places WHERE last_visit_date IS NOT NULL"
                           " AND hidden = 0 AND url LIKE 'http%' ORDER BY last_visit_date DESC LIMIT ?",
                           -1, &st, nullptr) != SQLITE_OK)
        return out;
    sqlite3_bind_int(st, 1, limit);
    while (sqlite3_step(st) == SQLITE_ROW)
        out.push_back({SqlText(st, 0), SqlText(st, 1), sqlite3_column_int64(st, 2) / 1000000});
    sqlite3_finalize(st);
    return out;
}

/// The default profile's directory from Firefox's profiles.ini: the [Install…] section's
/// Default= (current Firefox), else the [Profile…] marked Default=1. `relative` says
/// whether it is relative to the Firefox app-data folder. Empty if none.
inline std::string ParseFirefoxDefaultProfile(const std::string& ini, bool& relative) {
    std::string section, installDefault, profPath, profDefaultPath;
    bool profRelative = true, defRelative = true, isDefault = false;
    size_t pos = 0;
    auto endProfile = [&] {
        if (isDefault && !profPath.empty()) { profDefaultPath = profPath; defRelative = profRelative; }
        profPath.clear(); profRelative = true; isDefault = false;
    };
    while (pos <= ini.size()) {
        size_t nl = ini.find('\n', pos);
        std::string line = ini.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = nl == std::string::npos ? ini.size() + 1 : nl + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.empty()) continue;
        if (line[0] == '[') {
            if (section.rfind("Profile", 0) == 0) endProfile();
            section = line.substr(1, line.find(']') - 1);
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (section.rfind("Install", 0) == 0 && k == "Default" && installDefault.empty()) installDefault = v;
        if (section.rfind("Profile", 0) == 0) {
            if (k == "Path") profPath = v;
            else if (k == "IsRelative") profRelative = v != "0";
            else if (k == "Default") isDefault = v == "1";
        }
    }
    if (section.rfind("Profile", 0) == 0) endProfile();
    if (!installDefault.empty()) { relative = true; return installDefault; } // Install paths are relative
    relative = defRelative;
    return profDefaultPath;
}

// ---------------------------------------------------------------------------
// Merging into Minima's own lists.
// ---------------------------------------------------------------------------
/// Appends bookmarks whose URL Minima doesn't have yet; returns how many were added.
inline int MergeBookmarks(std::vector<BookmarkEntry>& mine, const std::vector<BookmarkEntry>& incoming) {
    int added = 0;
    for (auto& b : incoming) {
        bool have = std::any_of(mine.begin(), mine.end(), [&](const BookmarkEntry& m) { return m.url == b.url; });
        if (have) continue;
        mine.push_back(b);
        if (mine.back().title.empty()) mine.back().title = b.url;
        added++;
    }
    return added;
}

/// Merges visits (newest wins per URL), newest first, capped at `maxEntries`. Returns the
/// number of URLs that were new to Minima's history.
inline int MergeHistory(std::vector<HistoryEntry>& mine, const std::vector<HistoryEntry>& incoming, size_t maxEntries) {
    std::map<std::wstring, size_t> index;
    for (size_t i = 0; i < mine.size(); i++) index[mine[i].url] = i;
    int added = 0;
    for (auto& h : incoming) {
        auto it = index.find(h.url);
        if (it == index.end()) {
            index[h.url] = mine.size();
            mine.push_back(h);
            added++;
        } else if (h.time > mine[it->second].time) {
            mine[it->second].time = h.time;
            if (mine[it->second].title.empty()) mine[it->second].title = h.title;
        }
    }
    std::stable_sort(mine.begin(), mine.end(), [](const HistoryEntry& a, const HistoryEntry& b) { return a.time > b.time; });
    if (mine.size() > maxEntries) mine.resize(maxEntries);
    return added;
}

} // namespace minima
