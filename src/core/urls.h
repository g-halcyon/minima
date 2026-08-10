// urls.h — portable address-bar logic: decide whether input is a URL or a search, and
// build the destination URL (percent-encoding the query with a self-contained UTF-8
// encoder, so there's no dependency on the platform's string-conversion API).
// Part of the "core" per PORTING.md.
#pragma once

#include <cstdio>
#include <cwctype>
#include <string>

namespace minima {

/// Appends `c` to `out` as UTF-8 percent-escapes (%XX). Encodes a single UTF-16 BMP code
/// unit as a code point — matching the shell's previous WideCharToMultiByte behavior.
inline void AppendPercentEncoded(std::wstring& out, wchar_t c) {
    unsigned int cp = static_cast<unsigned int>(c);
    unsigned char bytes[4];
    int n = 0;
    if (cp < 0x80) {
        bytes[n++] = static_cast<unsigned char>(cp);
    } else if (cp < 0x800) {
        bytes[n++] = static_cast<unsigned char>(0xC0 | (cp >> 6));
        bytes[n++] = static_cast<unsigned char>(0x80 | (cp & 0x3F));
    } else {
        bytes[n++] = static_cast<unsigned char>(0xE0 | (cp >> 12));
        bytes[n++] = static_cast<unsigned char>(0x80 | ((cp >> 6) & 0x3F));
        bytes[n++] = static_cast<unsigned char>(0x80 | (cp & 0x3F));
    }
    for (int i = 0; i < n; i++) {
        wchar_t buf[8];
        swprintf(buf, 8, L"%%%02X", bytes[i]);
        out += buf;
    }
}

/// URL-vs-search heuristic. Returns the URL to navigate to: the input itself if it looks
/// like a URL, otherwise `searchUrlPrefix` + the percent-encoded query.
inline std::wstring ResolveInputToUrl(const std::wstring& input, const std::wstring& searchUrlPrefix) {
    bool hasSpace = input.find(L' ') != std::wstring::npos;
    bool hasDot = input.find(L'.') != std::wstring::npos;
    bool hasScheme = input.rfind(L"http://", 0) == 0 || input.rfind(L"https://", 0) == 0 ||
                     input.rfind(L"localhost", 0) == 0;
    if (hasScheme) return input.rfind(L"localhost", 0) == 0 ? L"http://" + input : input;
    if (hasDot && !hasSpace) return L"https://" + input;
    std::wstring encoded;
    for (wchar_t c : input) {
        if (iswalnum(c) || c == L'-' || c == L'_' || c == L'.' || c == L'~') encoded += c;
        else if (c == L' ') encoded += L'+';
        else AppendPercentEncoded(encoded, c);
    }
    return searchUrlPrefix + encoded;
}

} // namespace minima
