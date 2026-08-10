// win_services.h — the Windows implementations of Minima's platform services (the
// IHttpClient / IProcess / storage-IO seams from ../platform.h): file IO, WinHTTP
// networking/downloads, socket helpers, and child-process management. Everything here
// is Win32-only by design; the portable logic that *uses* these lives in src/core/.
// Other shells provide the same functions on their own APIs (see PORTING.md).
//
// Requires windows.h, winhttp.h, winsock2.h to be included first (main.cpp does).
#pragma once

#include <atomic>
#include <string>
#include <vector>

// ---- Storage IO (the IStorage seam): Minima's data files are UTF-16 on Windows ------

inline bool WriteWideFile(const std::wstring& path, const std::wstring& content) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written;
    WriteFile(h, content.data(), static_cast<DWORD>(content.size() * sizeof(wchar_t)), &written, nullptr);
    CloseHandle(h);
    return true;
}

inline std::wstring ReadWideFile(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return L"";
    DWORD size = GetFileSize(h, nullptr);
    std::wstring content;
    if (size > 0 && size != INVALID_FILE_SIZE) {
        content.resize(size / sizeof(wchar_t));
        DWORD read;
        ReadFile(h, content.data(), size, &read, nullptr);
    }
    CloseHandle(h);
    return content;
}

inline bool FileExists(const std::wstring& p) {
    return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES;
}

inline std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

/// Returns the directory that actually holds manifest.json (the root, or one level down —
/// some release zips wrap the extension in a subfolder). Empty if not found.
inline std::wstring FindManifestDir(const std::wstring& root) {
    if (FileExists(root + L"\\manifest.json")) return root;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((root + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && wcscmp(fd.cFileName, L".") &&
                wcscmp(fd.cFileName, L"..")) {
                std::wstring sub = root + L"\\" + fd.cFileName;
                if (FileExists(sub + L"\\manifest.json")) { FindClose(h); return sub; }
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return L"";
}

// ---- Networking (the IHttpClient seam): WinHTTP -------------------------------------

/// Small GET for API responses (JSON text); used only for the GitHub releases lookups.
inline std::string HttpGetText(const std::wstring& host, const std::wstring& path, DWORD* lastError = nullptr) {
    std::string result;
    HINTERNET hSession = WinHttpOpen(L"Minima/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) {
        if (lastError) *lastError = GetLastError();
        return result;
    }
    HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (hConnect) {
        HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                                WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        if (hRequest) {
            WinHttpAddRequestHeaders(hRequest, L"User-Agent: Minima-Browser\r\n", static_cast<DWORD>(-1),
                                     WINHTTP_ADDREQ_FLAG_ADD);
            if (WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                WinHttpReceiveResponse(hRequest, nullptr)) {
                DWORD avail = 0;
                do {
                    avail = 0;
                    if (!WinHttpQueryDataAvailable(hRequest, &avail) || avail == 0) break;
                    std::vector<char> buf(avail);
                    DWORD read = 0;
                    if (WinHttpReadData(hRequest, buf.data(), avail, &read)) result.append(buf.data(), read);
                } while (avail > 0);
            } else if (lastError) {
                *lastError = GetLastError();
            }
            WinHttpCloseHandle(hRequest);
        } else if (lastError) {
            *lastError = GetLastError();
        }
        WinHttpCloseHandle(hConnect);
    } else if (lastError) {
        *lastError = GetLastError();
    }
    WinHttpCloseHandle(hSession);
    return result;
}

/// Streams a URL to disk, following redirects (WinHTTP does this by default), reporting progress.
inline bool DownloadFile(const std::wstring& url, const std::wstring& destPath,
                         std::atomic<long long>* downloaded, std::atomic<long long>* total) {
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t hostBuf[256]{}, pathBuf[2048]{};
    uc.lpszHostName = hostBuf;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = pathBuf;
    uc.dwUrlPathLength = 2048;
    uc.dwSchemeLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) return false;
    bool tls = uc.nScheme == INTERNET_SCHEME_HTTPS;

    HINTERNET hSession = WinHttpOpen(L"Minima/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return false;
    bool ok = false;
    HINTERNET hConnect = WinHttpConnect(hSession, uc.lpszHostName, uc.nPort, 0);
    if (hConnect) {
        HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", uc.lpszUrlPath, nullptr, WINHTTP_NO_REFERER,
                                                WINHTTP_DEFAULT_ACCEPT_TYPES, tls ? WINHTTP_FLAG_SECURE : 0);
        if (hRequest) {
            if (WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                WinHttpReceiveResponse(hRequest, nullptr)) {
                DWORD statusCode = 0, statusSize = sizeof(statusCode);
                WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                    WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX);
                wchar_t lenBuf[32]{};
                DWORD lenSize = sizeof(lenBuf);
                if (total && WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX,
                                                 lenBuf, &lenSize, WINHTTP_NO_HEADER_INDEX)) {
                    total->store(_wcstoi64(lenBuf, nullptr, 10));
                }
                if (statusCode == 200) {
                    HANDLE hFile = CreateFileW(destPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                               FILE_ATTRIBUTE_NORMAL, nullptr);
                    if (hFile != INVALID_HANDLE_VALUE) {
                        std::vector<char> buf(65536);
                        DWORD avail = 0;
                        ok = true;
                        do {
                            avail = 0;
                            if (!WinHttpQueryDataAvailable(hRequest, &avail)) { ok = false; break; }
                            if (avail == 0) break;
                            DWORD toRead = std::min<DWORD>(avail, static_cast<DWORD>(buf.size()));
                            DWORD read = 0;
                            if (!WinHttpReadData(hRequest, buf.data(), toRead, &read)) { ok = false; break; }
                            DWORD written = 0;
                            WriteFile(hFile, buf.data(), read, &written, nullptr);
                            if (downloaded) *downloaded += read;
                        } while (avail > 0);
                        CloseHandle(hFile);
                    }
                }
            }
            WinHttpCloseHandle(hRequest);
        }
        WinHttpCloseHandle(hConnect);
    }
    WinHttpCloseHandle(hSession);
    return ok;
}

/// Binds an ephemeral localhost port to find one that's free, then releases it for llama-server to use.
inline int FindFreePort() {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    int port = 0;
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s != INVALID_SOCKET) {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
            int len = sizeof(addr);
            getsockname(s, reinterpret_cast<sockaddr*>(&addr), &len);
            port = ntohs(addr.sin_port);
        }
        closesocket(s);
    }
    WSACleanup();
    return port ? port : 8137;
}

/// Polls the llama-server's own /health endpoint — a listening TCP port doesn't mean the
/// model has finished loading and can actually answer requests yet.
inline bool CheckLocalHealth(int port) {
    HINTERNET hSession = WinHttpOpen(L"Minima/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME,
                                     WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return false;
    bool ok = false;
    HINTERNET hConnect = WinHttpConnect(hSession, L"127.0.0.1", static_cast<INTERNET_PORT>(port), 0);
    if (hConnect) {
        HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", L"/health", nullptr, WINHTTP_NO_REFERER,
                                                WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
        if (hRequest) {
            if (WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                WinHttpReceiveResponse(hRequest, nullptr)) {
                DWORD status = 0, size = sizeof(status);
                WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                    WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
                ok = (status == 200);
            }
            WinHttpCloseHandle(hRequest);
        }
        WinHttpCloseHandle(hConnect);
    }
    WinHttpCloseHandle(hSession);
    return ok;
}

// ---- Child processes (the IProcess seam) --------------------------------------------

/// Runs a command hidden and waits for it (used for tar.exe zip extraction).
inline bool RunAndWait(const std::wstring& cmdLine, DWORD timeoutMs) {
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> buf(cmdLine.begin(), cmdLine.end());
    buf.push_back(0);
    if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return false;
    WaitForSingleObject(pi.hProcess, timeoutMs);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return true;
}
