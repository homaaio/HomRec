/*
 * hr_version.cpp  —  HomRec centralised version registry
 *
 * Exports:
 *   hr_version_homrec()   → L"1.6.1"
 *   hr_version_core()     → L"1.4.3"
 *   hr_version_console()  → L"1.2.0"
 *   hr_version_full(buf, len)  → writes the !homrec --version string
 *
 * The Python bridge calls hr_version_full() when it sees  !homrec --version
 * and logs the result to the developer console.
 *
 * Build (MinGW-w64):
 *   g++ -O2 -std=c++17 -shared -static-libgcc -static-libstdc++ ^
 *       -o hr_version.dll hr_version.cpp
 */

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #define HR_EXPORT extern "C" __declspec(dllexport)
#else
  #define HR_EXPORT extern "C" __attribute__((visibility("default")))
#endif

#include <cstddef>
#include <cstring>
#include <cwchar>

/* ── Version strings ─────────────────────────────────────────────────────── */

static constexpr wchar_t k_ver_homrec[]  = L"1.6.1";
static constexpr wchar_t k_ver_core[]    = L"1.4.3";
static constexpr wchar_t k_ver_console[] = L"1.2.0";

/* Full banner printed by !homrec --version */
static constexpr wchar_t k_ver_banner[] =
    L"Version HomRec - 1.6.1, Core version - 1.4.3, Console version 1.2.0";

/* ── Exports ─────────────────────────────────────────────────────────────── */

HR_EXPORT const wchar_t *hr_version_homrec() {
    return k_ver_homrec;
}

HR_EXPORT const wchar_t *hr_version_core() {
    return k_ver_core;
}

HR_EXPORT const wchar_t *hr_version_console() {
    return k_ver_console;
}

/*
 * hr_version_full
 * Writes the full version banner into caller-supplied wchar_t buffer.
 * Returns number of characters written (0 on error).
 */
HR_EXPORT int hr_version_full(wchar_t *buf, int buf_chars) {
    if (!buf || buf_chars < 2) return 0;
    const int n = (int)wcslen(k_ver_banner);
    const int copy = (n < buf_chars - 1) ? n : buf_chars - 1;
    wmemcpy(buf, k_ver_banner, (size_t)copy);
    buf[copy] = L'\0';
    return copy;
}
