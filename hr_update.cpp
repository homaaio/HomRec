/*
 * hr_update.cpp  —  HomRec update checker  (v1.6.1)
 *
 * Replaces the Python check_for_updates() / _version_gt() functions.
 * Uses WinHTTP (Windows) or libcurl stub (non-Windows) to fetch the latest
 * GitHub release tag and compare it against CURRENT_VERSION.
 *
 * The check runs on a background thread so it never blocks the UI.
 * The result is delivered via a callback: CB_UPDATE(latest_tag_wstr).
 *
 * Build (MinGW-w64):
 *   g++ -O2 -std=c++17 -shared -static-libgcc -static-libstdc++ ^
 *       -o hr_update.dll hr_update.cpp -lwinhttp
 */

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #define NOMINMAX
  #include <windows.h>
  #include <winhttp.h>
  #pragma comment(lib, "winhttp.lib")
  #define HR_EXPORT extern "C" __declspec(dllexport)
#else
  #define HR_EXPORT extern "C" __attribute__((visibility("default")))
#endif

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include <sstream>
#include <algorithm>

/* ── Version ─────────────────────────────────────────────────────────────── */

static constexpr char  k_current[]   = "1.6.1";
static constexpr wchar_t k_repo[]    = L"homaaio/HomREC";
static constexpr wchar_t k_api_host[]= L"api.github.com";
static constexpr wchar_t k_api_path[]= L"/repos/homaaio/HomREC/releases/latest";

/* ── Callback type ───────────────────────────────────────────────────────── */

/* Called on a background thread when a newer version is found.
 * arg: null-terminated wchar_t string, e.g. L"1.6.2"            */
typedef void (*HR_UPDATE_CB)(const wchar_t *latest_tag);

/* ── Version comparison ──────────────────────────────────────────────────── */

static bool _version_gt(const std::string &a, const std::string &b) {
    auto parse = [](const std::string &s) -> std::vector<int> {
        std::vector<int> parts;
        std::stringstream ss(s);
        std::string tok;
        while (std::getline(ss, tok, '.'))
            parts.push_back(std::stoi(tok.empty() ? "0" : tok));
        return parts;
    };
    try {
        auto va = parse(a);
        auto vb = parse(b);
        size_t n = std::max(va.size(), vb.size());
        va.resize(n, 0); vb.resize(n, 0);
        for (size_t i = 0; i < n; ++i) {
            if (va[i] > vb[i]) return true;
            if (va[i] < vb[i]) return false;
        }
    } catch (...) {}
    return false;
}

/* ── Extract "tag_name" from minimal GitHub JSON ─────────────────────────── */

static std::string _extract_tag(const std::string &json) {
    const char *key = "\"tag_name\"";
    size_t pos = json.find(key);
    if (pos == std::string::npos) return {};
    pos += strlen(key);
    pos = json.find('"', pos);
    if (pos == std::string::npos) return {};
    ++pos;
    size_t end = json.find('"', pos);
    if (end == std::string::npos) return {};
    std::string tag = json.substr(pos, end - pos);
    /* strip leading 'v' */
    if (!tag.empty() && (tag[0] == 'v' || tag[0] == 'V')) tag = tag.substr(1);
    return tag;
}

/* ── HTTP fetch (WinHTTP) ────────────────────────────────────────────────── */

#ifdef _WIN32
static std::string _fetch_release_json() {
    std::string result;
    HINTERNET hSession = WinHttpOpen(
        L"HomRec/1.6.1 (update-check)",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return result;

    HINTERNET hConnect = WinHttpConnect(
        hSession, k_api_host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return result; }

    HINTERNET hReq = WinHttpOpenRequest(
        hConnect, L"GET", k_api_path,
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE);
    if (!hReq) {
        WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
        return result;
    }

    /* Set User-Agent and Accept headers */
    WinHttpAddRequestHeaders(hReq,
        L"User-Agent: HomRec/1.6.1\r\nAccept: application/json",
        (DWORD)-1L, WINHTTP_ADDREQ_FLAG_ADD);

    if (!WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(hReq, nullptr)) {
        WinHttpCloseHandle(hReq);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return result;
    }

    char buf[4096] = {};
    DWORD read = 0;
    while (WinHttpReadData(hReq, buf, sizeof(buf) - 1, &read) && read > 0) {
        buf[read] = '\0';
        result.append(buf, read);
        if (result.size() > 64 * 1024) break; /* sanity limit */
    }

    WinHttpCloseHandle(hReq);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return result;
}
#else
/* Non-Windows stub — always returns empty (update check disabled) */
static std::string _fetch_release_json() { return {}; }
#endif

/* ── Background thread entry ─────────────────────────────────────────────── */

struct _CheckCtx {
    HR_UPDATE_CB cb;
};

static void _check_thread(HR_UPDATE_CB cb) {
    try {
        std::string json = _fetch_release_json();
        if (json.empty()) return;
        std::string tag = _extract_tag(json);
        if (tag.empty()) return;
        if (!_version_gt(tag, k_current)) return;

        /* Convert tag to wchar for callback */
        std::wstring wtag(tag.begin(), tag.end());
        if (cb) cb(wtag.c_str());
    } catch (...) {}
}

/* ── Public API ───────────────────────────────────────────────────────────── */

/*
 * hr_update_check_async
 * Spawns a detached daemon thread that calls cb(tag) if a newer release
 * is found.  Returns immediately.
 */
HR_EXPORT void hr_update_check_async(HR_UPDATE_CB cb) {
    if (!cb) return;
    std::thread t([cb]{ _check_thread(cb); });
    t.detach();
}

/*
 * hr_update_current_version
 * Returns the compiled-in current version string.
 */
HR_EXPORT const char *hr_update_current_version() {
    return k_current;
}

/*
 * hr_update_version_gt
 * Returns 1 if version string a > b (dot-separated integers), else 0.
 * Exposed for testing / Python bridge.
 */
HR_EXPORT int hr_update_version_gt(const char *a, const char *b) {
    if (!a || !b) return 0;
    return _version_gt(std::string(a), std::string(b)) ? 1 : 0;
}
