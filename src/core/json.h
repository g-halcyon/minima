// json.h — portable JSON helpers for Minima's small flat data files
// (bookmarks/history/settings). Pure std C++, no platform dependencies — part of the
// "core" per PORTING.md. Extracted verbatim from the Windows shell; no behavior change.
//
// Note: still UTF-16 (std::wstring) to match the current Windows code. Migrating the
// core to UTF-8 std::string is a later roadmap item; the logic itself is portable now.
#pragma once

#include <cstdio>
#include <cstdlib>
#include <cwctype>
#include <string>
#include <utility>
#include <vector>

namespace minima {

inline std::wstring JsonEscape(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size() + 8);
    for (wchar_t c : s) {
        switch (c) {
            case L'"': out += L"\\\""; break;
            case L'\\': out += L"\\\\"; break;
            case L'\n': out += L"\\n"; break;
            case L'\r': out += L"\\r"; break;
            case L'\t': out += L"\\t"; break;
            default:
                if (c < 0x20) {
                    wchar_t buf[8];
                    swprintf(buf, 8, L"\\u%04x", c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

/// Minimal JSON reader for our own flat "array of {string/number fields}" files.
inline std::wstring ParseJsonString(const std::wstring& s, size_t& i) {
    std::wstring out;
    if (i >= s.size() || s[i] != L'"') return out;
    i++;
    while (i < s.size() && s[i] != L'"') {
        wchar_t c = s[i++];
        if (c == L'\\' && i < s.size()) {
            wchar_t e = s[i++];
            switch (e) {
                case L'"': out += L'"'; break;
                case L'\\': out += L'\\'; break;
                case L'/': out += L'/'; break;
                case L'n': out += L'\n'; break;
                case L'r': out += L'\r'; break;
                case L't': out += L'\t'; break;
                case L'u':
                    if (i + 4 <= s.size()) {
                        out += static_cast<wchar_t>(wcstol(s.substr(i, 4).c_str(), nullptr, 16));
                        i += 4;
                    }
                    break;
                default: out += e;
            }
        } else {
            out += c;
        }
    }
    if (i < s.size()) i++;
    return out;
}

inline std::wstring ParseJsonValue(const std::wstring& s, size_t& i) {
    while (i < s.size() && iswspace(s[i])) i++;
    if (i < s.size() && s[i] == L'"') return ParseJsonString(s, i);
    size_t start = i;
    while (i < s.size() && (iswdigit(s[i]) || s[i] == L'-' || s[i] == L'.')) i++;
    return s.substr(start, i - start);
}

inline bool ParseJsonObject(const std::wstring& s, size_t& i,
                            std::vector<std::pair<std::wstring, std::wstring>>& out) {
    while (i < s.size() && iswspace(s[i])) i++;
    if (i >= s.size() || s[i] != L'{') return false;
    i++;
    while (i < s.size()) {
        while (i < s.size() && (iswspace(s[i]) || s[i] == L',')) i++;
        if (i < s.size() && s[i] == L'}') { i++; return true; }
        if (i >= s.size() || s[i] != L'"') return true;
        std::wstring key = ParseJsonString(s, i);
        while (i < s.size() && (iswspace(s[i]) || s[i] == L':')) i++;
        std::wstring val = ParseJsonValue(s, i);
        out.emplace_back(std::move(key), std::move(val));
    }
    return true;
}

inline void ParseJsonArray(const std::wstring& s,
                           std::vector<std::vector<std::pair<std::wstring, std::wstring>>>& out) {
    size_t i = 0;
    while (i < s.size() && s[i] != L'[') i++;
    if (i >= s.size()) return;
    i++;
    while (i < s.size()) {
        while (i < s.size() && (iswspace(s[i]) || s[i] == L',')) i++;
        if (i >= s.size() || s[i] == L']') break;
        std::vector<std::pair<std::wstring, std::wstring>> obj;
        if (!ParseJsonObject(s, i, obj)) break;
        out.push_back(std::move(obj));
    }
}

} // namespace minima
