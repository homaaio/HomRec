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

/* -- Version strings ------------------------------------------------------- */

static constexpr wchar_t k_ver_homrec[]  = L"1.7.0";
static constexpr wchar_t k_ver_core[]    = L"1.5.0";
static constexpr wchar_t k_ver_console[] = L"1.3.0";

/* Full banner printed by !homrec --version */
static constexpr wchar_t k_ver_banner[] =
    L"Version HomRec - 1.7.0, Core version - 1.5.0, Console version 1.3.0";

/* -- Exports --------------------------------------------------------------- */

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
