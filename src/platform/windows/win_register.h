// win_register.h — per-user default-browser registration for Windows (no admin needed).
// Writes the ProgID + StartMenuInternet capabilities under HKCU so Minima appears in
// Windows' "Default apps" and can be chosen for http/https links and .htm/.html files.
// Windows deliberately doesn't let apps set themselves default programmatically; the
// Settings page registers and then opens ms-settings:defaultapps for the user to pick.
//
// Requires windows.h included first (main.cpp does). Uses advapi32 (already linked).
#pragma once

#include <string>

inline std::wstring ExePath() {
    wchar_t buf[MAX_PATH]{};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return buf;
}

inline void RegSetStr(const std::wstring& key, const wchar_t* name, const std::wstring& val) {
    RegSetKeyValueW(HKEY_CURRENT_USER, key.c_str(), name, REG_SZ, val.c_str(),
                    static_cast<DWORD>((val.size() + 1) * sizeof(wchar_t)));
}

inline bool IsBrowserRegistered() {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Clients\\StartMenuInternet\\Minima", 0,
                      KEY_READ, &k) == ERROR_SUCCESS) {
        RegCloseKey(k);
        return true;
    }
    return false;
}

inline void RegisterBrowser() {
    std::wstring exe = ExePath();
    std::wstring openCmd = L"\"" + exe + L"\" \"%1\"";
    std::wstring icon = exe + L",0";
    // ProgID used for URL/file associations.
    RegSetStr(L"Software\\Classes\\MinimaHTML", nullptr, L"Minima Web Document");
    RegSetStr(L"Software\\Classes\\MinimaHTML\\DefaultIcon", nullptr, icon);
    RegSetStr(L"Software\\Classes\\MinimaHTML\\shell\\open\\command", nullptr, openCmd);
    // Browser client + capabilities (what "Default apps" reads).
    const std::wstring base = L"Software\\Clients\\StartMenuInternet\\Minima";
    RegSetStr(base, nullptr, L"Minima");
    RegSetStr(base + L"\\DefaultIcon", nullptr, icon);
    RegSetStr(base + L"\\shell\\open\\command", nullptr, L"\"" + exe + L"\"");
    RegSetStr(base + L"\\Capabilities", L"ApplicationName", L"Minima");
    RegSetStr(base + L"\\Capabilities", L"ApplicationIcon", icon);
    RegSetStr(base + L"\\Capabilities", L"ApplicationDescription",
              L"An ultra-fast, minimal browser with fully on-device AI.");
    RegSetStr(base + L"\\Capabilities\\URLAssociations", L"http", L"MinimaHTML");
    RegSetStr(base + L"\\Capabilities\\URLAssociations", L"https", L"MinimaHTML");
    RegSetStr(base + L"\\Capabilities\\FileAssociations", L".htm", L"MinimaHTML");
    RegSetStr(base + L"\\Capabilities\\FileAssociations", L".html", L"MinimaHTML");
    RegSetStr(L"Software\\RegisteredApplications", L"Minima", base + L"\\Capabilities");
}

inline void UnregisterBrowser() {
    RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\Classes\\MinimaHTML");
    RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\Clients\\StartMenuInternet\\Minima");
    RegDeleteKeyValueW(HKEY_CURRENT_USER, L"Software\\RegisteredApplications", L"Minima");
}
