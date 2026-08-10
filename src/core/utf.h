// utf.h — portable wide↔UTF-8 conversion for the non-Windows shells (GTK/WebKit APIs
// take UTF-8; the core's strings are wchar_t). Handles both 2-byte (Windows) and 4-byte
// (Linux/macOS) wchar_t. Part of the "core" per PORTING.md.
#pragma once

#include <string>

namespace minima {

inline void AppendUtf8(std::string& out, unsigned int cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
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

inline std::string WideToUtf8(const std::wstring& w) {
    std::string out;
    out.reserve(w.size() * 3);
    for (size_t i = 0; i < w.size(); i++) {
        unsigned int cp = static_cast<unsigned int>(w[i]);
        // Combine UTF-16 surrogate pairs (2-byte wchar_t platforms).
        if (sizeof(wchar_t) == 2 && cp >= 0xD800 && cp <= 0xDBFF && i + 1 < w.size()) {
            unsigned int lo = static_cast<unsigned int>(w[i + 1]);
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                i++;
            }
        }
        AppendUtf8(out, cp);
    }
    return out;
}

inline std::wstring Utf8ToWideStr(const std::string& s) {
    std::wstring out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        unsigned int cp = 0;
        int extra = 0;
        if (c < 0x80) { cp = c; }
        else if ((c >> 5) == 0x6) { cp = c & 0x1F; extra = 1; }
        else if ((c >> 4) == 0xE) { cp = c & 0x0F; extra = 2; }
        else if ((c >> 3) == 0x1E) { cp = c & 0x07; extra = 3; }
        else { i++; continue; } // invalid lead byte — skip
        if (i + extra >= s.size() + (extra ? 0 : 1)) break;
        bool ok = true;
        for (int k = 1; k <= extra; k++) {
            unsigned char cc = static_cast<unsigned char>(s[i + k]);
            if ((cc >> 6) != 0x2) { ok = false; break; }
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (!ok) { i++; continue; }
        i += 1 + extra;
        if (sizeof(wchar_t) == 2 && cp >= 0x10000) {
            cp -= 0x10000;
            out += static_cast<wchar_t>(0xD800 + (cp >> 10));
            out += static_cast<wchar_t>(0xDC00 + (cp & 0x3FF));
        } else {
            out += static_cast<wchar_t>(cp);
        }
    }
    return out;
}

} // namespace minima
