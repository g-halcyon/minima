// memory.h — Minima's Memory: a private, on-device full-text index of the pages you've
// read, so "that article about X last week" is findable. SQLite FTS5 (vendored, public
// domain): porter stemming + diacritic folding + prefix index for search-as-you-type,
// BM25 ranking with title matches weighted up. Nothing here touches the network. Part of
// the portable "core" per PORTING.md (plain C++17 + sqlite3).
#pragma once

#include <cwctype>
#include <initializer_list>
#include <string>
#include <vector>

#include "sqlite3.h"
#include "utf.h"

namespace minima {

struct MemoryHit {
    std::wstring url, title, site;
    std::wstring snippet; // matched terms wrapped in \x02 … \x03 (the UI turns them into <b>)
    long long visited = 0;
};

/// FTS5 query for free text typed by the user. Every word is required; each matches either
/// as a whole (stemmed: "eviction" finds "evicted") or as a prefix of a longer word (search
/// as you type: "micro" finds "microcontrollers") — the index is stemmed, so a bare prefix
/// alone would miss inflected forms. Short words match exactly (a 2-letter prefix matches
/// almost everything) and common English filler words are dropped unless that leaves
/// nothing. Only letters/digits survive, so user input can never inject FTS operators.
inline std::wstring BuildFtsQuery(const std::wstring& q) {
    static const wchar_t* const kStop[] = {L"a", L"an", L"and", L"are", L"as", L"at", L"be", L"by", L"for", L"from",
                                           L"how", L"in", L"is", L"it", L"of", L"on", L"or", L"that", L"the", L"this",
                                           L"to", L"was", L"what", L"when", L"where", L"which", L"who", L"why", L"with"};
    std::vector<std::wstring> words, kept;
    std::wstring tok;
    auto flush = [&] {
        if (tok.size() >= 2 && words.size() < 8) words.push_back(tok);
        tok.clear();
    };
    for (wchar_t c : q) {
        if (iswalnum(c) || c > 0x2E7F) tok += static_cast<wchar_t>(towlower(c)); // keep CJK etc. too
        else flush();
    }
    flush();
    for (auto& w : words) {
        bool stop = false;
        for (auto s : kStop) stop = stop || w == s;
        if (!stop) kept.push_back(w);
    }
    if (kept.empty()) kept = words;
    std::wstring out;
    for (auto& w : kept) {
        if (!out.empty()) out += L" AND "; // FTS5 needs it explicit between (..) groups
        out += w.size() >= 3 ? L"(\"" + w + L"\" OR \"" + w + L"\"*)" : L"\"" + w + L"\"";
    }
    return out;
}

class MemoryStore {
public:
    ~MemoryStore() { Close(); }

    /// Opens (creating if needed) the index at a UTF-8 path. Cheap; safe to call again.
    bool Open(const std::string& path) {
        if (db_) return true;
        if (sqlite3_open_v2(path.c_str(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK) {
            Close();
            return false;
        }
        sqlite3_busy_timeout(db_, 2000);
        Exec("PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;");
        bool ok = Exec(
            "CREATE TABLE IF NOT EXISTS docs(id INTEGER PRIMARY KEY, url TEXT UNIQUE NOT NULL, title TEXT,"
            " site TEXT, visited INTEGER, words INTEGER);"
            "CREATE INDEX IF NOT EXISTS docs_visited ON docs(visited);"
            "CREATE VIRTUAL TABLE IF NOT EXISTS fts USING fts5(title, body,"
            " tokenize='porter unicode61 remove_diacritics 2', prefix='2 3');");
        if (!ok) Close();
        return ok;
    }

    void Close() {
        if (db_) sqlite3_close(db_);
        db_ = nullptr;
    }

    bool IsOpen() const { return db_ != nullptr; }

    /// Remembers (or refreshes) a page. Revisits replace the old text and move it to "now".
    bool Add(const std::wstring& url, const std::wstring& title, const std::wstring& site,
             const std::wstring& text, long long visited, int words) {
        if (!db_) return false;
        std::string u = WideToUtf8(url), t = WideToUtf8(title), s = WideToUtf8(site), b = WideToUtf8(text);
        Exec("BEGIN");
        long long id = -1;
        if (sqlite3_stmt* st = Prepare("SELECT id FROM docs WHERE url=?")) {
            sqlite3_bind_text(st, 1, u.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(st) == SQLITE_ROW) id = sqlite3_column_int64(st, 0);
            sqlite3_finalize(st);
        }
        bool ok = true;
        if (id >= 0) {
            ok = Run("UPDATE docs SET title=?, site=?, visited=?, words=? WHERE id=?", {t, s}, {visited, words, id}) &&
                 Run("DELETE FROM fts WHERE rowid=?", {}, {id});
        } else {
            ok = Run("INSERT INTO docs(url, title, site, visited, words) VALUES(?,?,?,?,?)", {u, t, s}, {visited, words});
            id = sqlite3_last_insert_rowid(db_);
        }
        if (ok) {
            sqlite3_stmt* st = Prepare("INSERT INTO fts(rowid, title, body) VALUES(?,?,?)");
            ok = st != nullptr;
            if (st) {
                sqlite3_bind_int64(st, 1, id);
                sqlite3_bind_text(st, 2, t.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(st, 3, b.c_str(), -1, SQLITE_TRANSIENT);
                ok = sqlite3_step(st) == SQLITE_DONE;
                sqlite3_finalize(st);
            }
        }
        Exec(ok ? "COMMIT" : "ROLLBACK");
        return ok;
    }

    /// Best matches for free text, most relevant first.
    std::vector<MemoryHit> Search(const std::wstring& query, int limit) {
        std::vector<MemoryHit> hits;
        std::string fts = WideToUtf8(BuildFtsQuery(query));
        if (!db_ || fts.empty()) return hits;
        sqlite3_stmt* st = Prepare(
            "SELECT d.url, d.title, d.site, d.visited, snippet(fts, 1, char(2), char(3), '\xE2\x80\xA6', 14)"
            " FROM fts JOIN docs d ON d.id = fts.rowid WHERE fts MATCH ?"
            " ORDER BY bm25(fts, 4.0, 1.0) LIMIT ?");
        if (!st) return hits;
        sqlite3_bind_text(st, 1, fts.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(st, 2, limit);
        while (sqlite3_step(st) == SQLITE_ROW) {
            MemoryHit h;
            h.url = Col(st, 0);
            h.title = Col(st, 1);
            h.site = Col(st, 2);
            h.visited = sqlite3_column_int64(st, 3);
            h.snippet = Col(st, 4);
            hits.push_back(std::move(h));
        }
        sqlite3_finalize(st);
        return hits;
    }

    void Forget(const std::wstring& url) {
        long long id = IdFor(WideToUtf8(url));
        if (id < 0) return;
        Run("DELETE FROM fts WHERE rowid=?", {}, {id});
        Run("DELETE FROM docs WHERE id=?", {}, {id});
    }

    /// Forgets every page whose site is `host` or a subdomain of it.
    void ForgetSite(const std::wstring& host) {
        std::string h = WideToUtf8(host), sub = "%." + h;
        const char* where = "url LIKE 'http://' || ?1 || '/%' OR url LIKE 'https://' || ?1 || '/%'"
                            " OR url LIKE 'http://' || ?2 || '/%' OR url LIKE 'https://' || ?2 || '/%'";
        Run((std::string("DELETE FROM fts WHERE rowid IN (SELECT id FROM docs WHERE ") + where + ")").c_str(), {h, sub}, {});
        Run((std::string("DELETE FROM docs WHERE ") + where).c_str(), {h, sub}, {});
    }

    void Clear() {
        if (!db_) return;
        Exec("DELETE FROM fts; DELETE FROM docs;");
        Exec("VACUUM");
    }

    long long Count() {
        long long n = 0;
        if (sqlite3_stmt* st = Prepare("SELECT count(*) FROM docs")) {
            if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int64(st, 0);
            sqlite3_finalize(st);
        }
        return n;
    }

    /// Keeps only the `maxDocs` most recently visited pages.
    void Prune(long long maxDocs) {
        if (Count() <= maxDocs) return;
        const char* old = "SELECT id FROM docs ORDER BY visited DESC LIMIT -1 OFFSET ?";
        Run((std::string("DELETE FROM fts WHERE rowid IN (") + old + ")").c_str(), {}, {maxDocs});
        Run((std::string("DELETE FROM docs WHERE id IN (") + old + ")").c_str(), {}, {maxDocs});
    }

private:
    sqlite3* db_ = nullptr;

    bool Exec(const char* sql) { return db_ && sqlite3_exec(db_, sql, nullptr, nullptr, nullptr) == SQLITE_OK; }

    sqlite3_stmt* Prepare(const char* sql) {
        sqlite3_stmt* st = nullptr;
        if (!db_ || sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return nullptr;
        return st;
    }

    /// Runs a statement binding the text parameters first, then the integers.
    bool Run(const char* sql, std::initializer_list<std::string> texts, std::initializer_list<long long> ints) {
        sqlite3_stmt* st = Prepare(sql);
        if (!st) return false;
        int i = 1;
        for (auto& t : texts) sqlite3_bind_text(st, i++, t.c_str(), -1, SQLITE_TRANSIENT);
        for (long long v : ints) sqlite3_bind_int64(st, i++, v);
        int rc = sqlite3_step(st);
        sqlite3_finalize(st);
        return rc == SQLITE_DONE || rc == SQLITE_ROW;
    }

    long long IdFor(const std::string& url) {
        long long id = -1;
        if (sqlite3_stmt* st = Prepare("SELECT id FROM docs WHERE url=?")) {
            sqlite3_bind_text(st, 1, url.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(st) == SQLITE_ROW) id = sqlite3_column_int64(st, 0);
            sqlite3_finalize(st);
        }
        return id;
    }

    static std::wstring Col(sqlite3_stmt* st, int i) {
        const unsigned char* p = sqlite3_column_text(st, i);
        return Utf8ToWideStr(p ? reinterpret_cast<const char*>(p) : "");
    }
};

} // namespace minima
