/*
 * hr_tools.cpp — HomRec Tools Engine
 *
 * Exported (C ABI, hr_tools.dll):
 *
 *   hr_check_ffmpeg (hint_path, out_path, out_len) -> 1/0
 *   hr_get_dshow_devices (ffmpeg_path, out_buf, buf_chars) -> device count
 *   hr_probe_gpu (ffmpeg_path, out_encoder, out_len) -> 1/0
 *   hr_build_codec_args (codec, quality, fps, cpu_count, out_buf, buf_chars) -> token count
 *   hr_merge_av (ffmpeg_path, video_file, audio_file) -> 1/0
 *
 * All strings are wchar_t (UTF-16) for seamless ctypes on Windows.
 */

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <string>
#include <vector>
#include <sstream>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cctype>

#ifdef _WIN32
  #define HR_EXPORT extern "C" __declspec(dllexport)
#else
  #define HR_EXPORT extern "C" __attribute__((visibility("default")))
#endif

// ─────────────────────────────────────────────────────────────
// Internal: run a command, return combined stdout+stderr output
// ─────────────────────────────────────────────────────────────
static std::wstring run_cmd(const std::wstring& cmd, DWORD timeout_ms = 8000)
{
    SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE hRead = nullptr, hWrite = nullptr;
    if (!CreatePipe(&hRead, &hWrite, &sa, 0)) return {};
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.hStdOutput = hWrite;
    si.hStdError  = hWrite;
    si.dwFlags    = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi{};
    std::wstring mut_cmd = cmd;
    if (!CreateProcessW(nullptr, mut_cmd.data(),
                        nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
    {
        CloseHandle(hRead); CloseHandle(hWrite);
        return {};
    }
    CloseHandle(hWrite);
    WaitForSingleObject(pi.hProcess, timeout_ms);

    std::string raw;
    char buf[4096]; DWORD br = 0;
    while (ReadFile(hRead, buf, sizeof(buf)-1, &br, nullptr) && br)
    { buf[br] = '\0'; raw += buf; }

    TerminateProcess(pi.hProcess, 0);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread); CloseHandle(hRead);

    if (raw.empty()) return {};
    int wl = MultiByteToWideChar(CP_UTF8, 0, raw.c_str(), -1, nullptr, 0);
    std::wstring res(wl, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, raw.c_str(), -1, res.data(), wl);
    return res;
}

static bool fexists(const std::wstring& p)
{
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static bool probe_ff(const std::wstring& path, wchar_t* out, int len)
{
    if (!fexists(path)) return false;
    std::wstring out_txt = run_cmd(L"\"" + path + L"\" -version", 4000);
    if (out_txt.find(L"ffmpeg version") == std::wstring::npos) return false;
    wcsncpy_s(out, len, path.c_str(), _TRUNCATE);
    return true;
}

// ─────────────────────────────────────────────────────────────
// hr_check_ffmpeg
// ─────────────────────────────────────────────────────────────
HR_EXPORT int hr_check_ffmpeg(const wchar_t* hint, wchar_t* out, int out_len)
{
    if (hint && *hint && probe_ff(hint, out, out_len)) return 1;

    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (auto* s = wcsrchr(exe, L'\\')) *s = L'\0';
    std::wstring d(exe);

    for (auto& c : std::vector<std::wstring>{
            d + L"\\ffmpeg.exe",
            d + L"\\bin\\ffmpeg.exe",
            d + L"\\ffmpeg\\ffmpeg.exe",
            L"C:\\ffmpeg\\bin\\ffmpeg.exe",
            L"C:\\Program Files\\ffmpeg\\bin\\ffmpeg.exe",
            L"C:\\tools\\ffmpeg\\bin\\ffmpeg.exe" })
        if (probe_ff(c, out, out_len)) return 1;

    // PATH lookup via where
    std::wstring w = run_cmd(L"where ffmpeg", 3000);
    std::wistringstream ss(w); std::wstring line;
    while (std::getline(ss, line)) {
        while (!line.empty() && (line.back()==L'\r'||line.back()==L'\n')) line.pop_back();
        if (!line.empty() && probe_ff(line, out, out_len)) return 1;
    }
    return 0;
}

// ─────────────────────────────────────────────────────────────
// hr_get_dshow_devices
// ─────────────────────────────────────────────────────────────
HR_EXPORT int hr_get_dshow_devices(const wchar_t* ffpath,
                                    wchar_t* out, int out_len)
{
    if (!ffpath || !*ffpath) return 0;
    std::wstring raw = run_cmd(
        L"\"" + std::wstring(ffpath) + L"\" -list_devices true -f dshow -i dummy",
        6000);

    std::vector<std::wstring> devs;
    std::wistringstream ss(raw); std::wstring line;
    while (std::getline(ss, line)) {
        std::wstring low = line;
        std::transform(low.begin(), low.end(), low.begin(), ::towlower);
        bool is_audio = low.find(L"audio")  != std::wstring::npos
                     || low.find(L"stereo") != std::wstring::npos
                     || low.find(L"mix")    != std::wstring::npos
                     || low.find(L"\xf7\xf2\xe5\xf0\xe5\xee") != std::wstring::npos; // "стерео" cp1251 fallback
        if (!is_audio) continue;
        size_t q1 = line.find(L'"');
        if (q1 == std::wstring::npos) continue;
        size_t q2 = line.find(L'"', q1+1);
        if (q2 == std::wstring::npos) continue;
        std::wstring name = line.substr(q1+1, q2-q1-1);
        if (!name.empty()) devs.push_back(name);
    }
    if (devs.empty()) return 0;

    std::wstring joined;
    for (size_t i=0; i<devs.size(); i++) { if(i) joined+=L'\n'; joined+=devs[i]; }
    wcsncpy_s(out, out_len, joined.c_str(), _TRUNCATE);
    return (int)devs.size();
}

// ─────────────────────────────────────────────────────────────
// hr_probe_gpu  (run on background thread from Python)
// ─────────────────────────────────────────────────────────────
struct GpuCand { const wchar_t* name; const wchar_t* extra_args; };
static const GpuCand k_gpu[] = {
    { L"h264_nvenc", L" -f lavfi -i nullsrc=s=32x32:d=0.1 -c:v h264_nvenc -f null -" },
    { L"h264_amf",   L" -f lavfi -i nullsrc=s=32x32:d=0.1 -c:v h264_amf   -f null -" },
    { L"h264_qsv",   L" -f lavfi -i nullsrc=s=32x32:d=0.1 -c:v h264_qsv   -f null -" },
    { nullptr, nullptr }
};

HR_EXPORT int hr_probe_gpu(const wchar_t* ffpath, wchar_t* out_enc, int out_len)
{
    if (!ffpath || !*ffpath) return 0;
    for (const GpuCand* c = k_gpu; c->name; c++) {
        std::wstring cmd = L"\"" + std::wstring(ffpath) + L"\" -y" + c->extra_args;
        std::wstring res = run_cmd(cmd, 10000);
        std::wstring low = res;
        std::transform(low.begin(), low.end(), low.begin(), ::towlower);
        bool fail = low.find(L"no such encoder")   != std::wstring::npos
                 || low.find(L"encoder not found") != std::wstring::npos
                 || low.find(L"failed to")         != std::wstring::npos
                 || low.find(L"conversion failed") != std::wstring::npos;
        if (!fail) {
            wcsncpy_s(out_enc, out_len, c->name, _TRUNCATE);
            return 1;
        }
    }
    if (out_len > 0) out_enc[0] = L'\0';
    return 0;
}

// ─────────────────────────────────────────────────────────────
// hr_build_codec_args
// Returns space-separated ffmpeg argument string in out_buf.
// ─────────────────────────────────────────────────────────────
HR_EXPORT int hr_build_codec_args(const wchar_t* codec,
                                   int quality, int fps, int cpu_count,
                                   wchar_t* out_buf, int buf_chars)
{
    if (!codec || !out_buf || buf_chars < 2) return 0;

    // quality 0-100 → qp/crf in [23,34]
    int qp  = 34 - (int)((quality / 100.0) * 11);
    if (qp < 23) qp = 23;
    if (qp > 34) qp = 34;
    int gop = fps * 2;

    std::wstring c(codec);
    bool is_nvenc = c.find(L"nvenc") != std::wstring::npos;
    bool is_qsv   = c.find(L"qsv")   != std::wstring::npos;
    bool is_amf   = c.find(L"amf")   != std::wstring::npos;
    bool is_265   = c == L"libx265" || c.find(L"hevc") != std::wstring::npos;

    std::wostringstream ss;
    ss << L"-c:v " << codec;

    if (is_nvenc) {
        ss << L" -preset p1 -tune ull -rc constqp -qp " << qp << L" -g " << gop;
    } else if (is_qsv) {
        ss << L" -preset veryfast -look_ahead 0 -low_power 1 -qp " << qp << L" -g " << gop;
    } else if (is_amf) {
        ss << L" -quality speed -rc cqp -qp_i " << qp << L" -qp_p " << qp << L" -g " << gop;
    } else {
        int thr = (cpu_count <= 4) ? 1 : std::max(1, cpu_count/4);
        ss << L" -preset ultrafast -tune zerolatency -crf " << qp
           << L" -g " << gop << L" -threads " << thr;
        if (is_265) ss << L" -x265-params log-level=error";
    }

    std::wstring result = ss.str();
    wcsncpy_s(out_buf, buf_chars, result.c_str(), _TRUNCATE);

    // Count tokens
    int tok = 0; bool in_tok = false;
    for (wchar_t ch : result) {
        if (ch==L' ') { in_tok=false; }
        else { if (!in_tok) tok++; in_tok=true; }
    }
    return tok;
}

// ─────────────────────────────────────────────────────────────
// hr_merge_av
// ─────────────────────────────────────────────────────────────
HR_EXPORT int hr_merge_av(const wchar_t* ffpath,
                           const wchar_t* video_file,
                           const wchar_t* audio_file)
{
    if (!ffpath || !video_file || !audio_file) return 0;

    std::wstring vf(video_file);
    // Build temp output path
    std::wstring tmp = vf;
    size_t dot = tmp.rfind(L'.');
    if (dot != std::wstring::npos)
        tmp = tmp.substr(0, dot) + L"_mrgtmp" + tmp.substr(dot);
    else
        tmp += L"_mrgtmp.mp4";

    std::wstring cmd =
        L"\"" + std::wstring(ffpath) + L"\""
        L" -i \"" + vf + L"\""
        L" -i \"" + std::wstring(audio_file) + L"\""
        L" -c:v copy -c:a aac"
        L" -af aresample=async=1000"
        L" -map 0:v:0 -map 1:a:0"
        L" -shortest -y"
        L" \"" + tmp + L"\"";

    run_cmd(cmd, 180000);

    if (!fexists(tmp)) return 0;

    DeleteFileW(video_file);
    if (!MoveFileW(tmp.c_str(), video_file)) {
        CopyFileW(tmp.c_str(), video_file, FALSE);
        DeleteFileW(tmp.c_str());
    }
    return fexists(vf) ? 1 : 0;
}
