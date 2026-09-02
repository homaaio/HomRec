// hr_update.cpp

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>
#include <shellapi.h>
#pragma comment(lib, "winhttp.lib")

#include "hr_update.h"
#include "ui/version.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>
#include <vector>

namespace HrUpdate {

namespace {

constexpr wchar_t kApiHost[] = L"api.github.com";
constexpr wchar_t kApiPath[] = L"/repos/homaaio/HomREC/releases/latest";
constexpr wchar_t kUserAgent[] = L"HomRec-UpdateCheck/" HR_APP_VERSION_W;

// Plain substring search for `"key":"value"` (or `"key": "value"` - one
// optional space after the colon is all GitHub's API ever emits). Returns
// true and fills `out` on success; leaves `out` untouched on failure.
bool ExtractString(const std::string &json, const char *key, size_t from, size_t *found_at, std::string *out) {
    std::string needle = std::string("\"") + key + "\"";
    size_t pos = json.find(needle, from);
    if (pos == std::string::npos) return false;
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return false;
    pos = json.find('"', pos);
    if (pos == std::string::npos) return false;
    ++pos;
    size_t end = json.find('"', pos);
    if (end == std::string::npos) return false;
    *out = json.substr(pos, end - pos);
    if (found_at) *found_at = end;
    return true;
}

// Downloads the whole response body of an HTTPS GET as a string. Empty
// string on any failure (caller treats that as "check failed").
std::string HttpsGetText(const wchar_t *host, const wchar_t *path) {
    std::string result;
    HINTERNET hSession = WinHttpOpen(kUserAgent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return result;
    HINTERNET hConnect = WinHttpConnect(hSession, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return result; }
    HINTERNET hReq = WinHttpOpenRequest(hConnect, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                         WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!hReq) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return result; }

    WinHttpAddRequestHeaders(hReq, L"Accept: application/vnd.github+json", (DWORD)-1L, WINHTTP_ADDREQ_FLAG_ADD);

    if (WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(hReq, nullptr)) {
        char buf[4096];
        DWORD read = 0;
        while (WinHttpReadData(hReq, buf, sizeof(buf), &read) && read > 0) {
            result.append(buf, read);
            if (result.size() > 256 * 1024) break; // sanity limit - a release JSON is a few KB
        }
    }

    WinHttpCloseHandle(hReq);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return result;
}

// Downloads a URL's raw bytes straight to disk. `url` must be https.
// Returns true on success (status 200 and at least one byte written).
bool DownloadToFile(const std::wstring &url, const std::wstring &dest_path) {
    URL_COMPONENTS uc = {};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {}, path[2048] = {};
    uc.lpszHostName = host; uc.dwHostNameLength = _countof(host);
    uc.lpszUrlPath = path; uc.dwUrlPathLength = _countof(path);
    if (!WinHttpCrackUrl(url.c_str(), (DWORD)url.size(), 0, &uc)) return false;

    HINTERNET hSession = WinHttpOpen(kUserAgent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return false;
    HINTERNET hConnect = WinHttpConnect(hSession, host, uc.nPort, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return false; }
    HINTERNET hReq = WinHttpOpenRequest(hConnect, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                         WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    if (!hReq) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return false; }

    // GitHub asset downloads 302-redirect to a signed S3/Azure URL - WinHTTP
    // follows redirects by default, but the *host* the request was opened
    // against still governs which security flags apply, so we ask nothing
    // release-specific of the redirected host beyond "give me the bytes".
    bool ok = false;
    if (WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(hReq, nullptr)) {
        DWORD status = 0, statusLen = sizeof(status);
        WinHttpQueryHeaders(hReq, WINHTTP_QUERY_FLAG_NUMBER | WINHTTP_QUERY_STATUS_CODE,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusLen, WINHTTP_NO_HEADER_INDEX);
        if (status == 200) {
            std::ofstream out(std::filesystem::path(dest_path), std::ios::binary | std::ios::trunc);
            if (out.is_open()) {
                char buf[8192];
                DWORD read = 0;
                size_t total = 0;
                while (WinHttpReadData(hReq, buf, sizeof(buf), &read) && read > 0) {
                    out.write(buf, read);
                    total += read;
                }
                out.close();
                ok = total > 0;
            }
        }
    }

    WinHttpCloseHandle(hReq);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return ok;
}

// Finds the first "assets[].browser_download_url" whose "name" ends in
// .exe (case-insensitive) - i.e. the Inno Setup installer, as opposed to
// the auto-generated "Source code (zip/tar.gz)" links every release also
// gets. Scans name/url pairs in whatever order the API returned them.
std::string FindInstallerAssetUrl(const std::string &json) {
    size_t pos = 0;
    for (;;) {
        std::string name;
        size_t nameEnd = 0;
        if (!ExtractString(json, "name", pos, &nameEnd, &name)) break;
        pos = nameEnd;

        bool isExe = name.size() > 4;
        if (isExe) {
            std::string tail = name.substr(name.size() - 4);
            std::transform(tail.begin(), tail.end(), tail.begin(), [](unsigned char c) { return (char)tolower(c); });
            isExe = tail == ".exe";
        }
        if (isExe) {
            std::string url;
            size_t urlEnd = 0;
            // browser_download_url for this asset sits shortly after its
            // "name" field in GitHub's JSON - bound the search so we don't
            // accidentally grab the *next* asset's URL if this one has none.
            size_t nextName = json.find("\"name\"", pos);
            std::string window = json.substr(pos, nextName == std::string::npos ? std::string::npos : nextName - pos);
            if (ExtractString(window, "browser_download_url", 0, nullptr, &url)) return url;
        }
    }
    return {};
}

} // namespace

bool VersionGreaterThan(const std::string &a, const std::string &b) {
    auto parse = [](const std::string &s) {
        std::vector<int> parts;
        std::stringstream ss(s);
        std::string tok;
        while (std::getline(ss, tok, '.')) {
            try { parts.push_back(std::stoi(tok.empty() ? "0" : tok)); }
            catch (...) { parts.push_back(0); }
        }
        return parts;
    };
    std::vector<int> va = parse(a), vb = parse(b);
    size_t n = std::max(va.size(), vb.size());
    va.resize(n, 0); vb.resize(n, 0);
    for (size_t i = 0; i < n; ++i) {
        if (va[i] != vb[i]) return va[i] > vb[i];
    }
    return false;
}

UpdateInfo CheckForUpdate() {
    UpdateInfo info;
    std::string json = HttpsGetText(kApiHost, kApiPath);
    if (json.empty()) return info; // checked_ok stays false

    std::string tag;
    if (!ExtractString(json, "tag_name", 0, nullptr, &tag)) return info;
    if (!tag.empty() && (tag[0] == 'v' || tag[0] == 'V')) tag = tag.substr(1);

    std::string htmlUrl;
    ExtractString(json, "html_url", 0, nullptr, &htmlUrl);

    info.checked_ok = true;
    info.latest_version = tag;
    info.html_url = htmlUrl;
    info.available = VersionGreaterThan(tag, HR_APP_VERSION);
    if (info.available) info.download_url = FindInstallerAssetUrl(json);
    return info;
}

void CheckForUpdateAsync(std::function<void(UpdateInfo)> on_done) {
    if (!on_done) return;
    std::thread([on_done]() { on_done(CheckForUpdate()); }).detach();
}

bool DownloadAndLaunchInstaller(const UpdateInfo &info) {
    if (info.download_url.empty()) return false;

    int wlen = MultiByteToWideChar(CP_UTF8, 0, info.download_url.c_str(), -1, nullptr, 0);
    if (wlen <= 0) return false;
    std::wstring url(wlen - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, info.download_url.c_str(), -1, url.data(), wlen);

    wchar_t tempDir[MAX_PATH] = {};
    if (!GetTempPathW(MAX_PATH, tempDir)) return false;
    std::wstring destPath = std::wstring(tempDir) + L"HomRec-Update-" + std::wstring(info.latest_version.begin(), info.latest_version.end()) + L".exe";

    if (!DownloadToFile(url, destPath)) return false;

    // Inno Setup silent-upgrade flags: /VERYSILENT suppresses all UI,
    // /SUPPRESSMSGBOXES avoids any "are you sure" prompts, /NORESTART
    // stops it rebooting the machine on our behalf, and /CLOSEAPPLICATIONS
    // (paired with AppMutex/CloseApplications in installer/HomRec.iss)
    // lets it close this very process mid-launch instead of failing to
    // overwrite hr.exe because it's in use.
    HINSTANCE result = ShellExecuteW(nullptr, L"open", destPath.c_str(),
                                      L"/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /CLOSEAPPLICATIONS",
                                      nullptr, SW_SHOWNORMAL);
    return (INT_PTR)result > 32;
}

} // namespace HrUpdate