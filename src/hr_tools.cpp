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
#include <thread>
#include "hr_log.h"

#ifdef _WIN32
  #define HR_EXPORT extern "C" __declspec(dllexport)
#else
  #define HR_EXPORT extern "C" __attribute__((visibility("default")))
#endif

// -------------------------------------------------------------
// Internal: run a command, return combined stdout+stderr output
// -------------------------------------------------------------
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

    // Drain the pipe concurrently with waiting for the process to exit -
    // see the comment above run_cmd() for why reading only *after*
    // WaitForSingleObject deadlocks as soon as the child's combined
    // stdout+stderr output exceeds the pipe's small default buffer.
    std::string raw;
    std::thread reader([&]() {
        char buf[4096]; DWORD br = 0;
        while (ReadFile(hRead, buf, sizeof(buf) - 1, &br, nullptr) && br) {
            buf[br] = '\0';
            raw += buf;
        }
    });

    WaitForSingleObject(pi.hProcess, timeout_ms);
    TerminateProcess(pi.hProcess, 0); // harmless no-op if it already exited
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);

    // The process exiting (naturally or via TerminateProcess just above)
    // closes its inherited handle to the write end, which is what lets
    // the reader thread's ReadFile loop see EOF and return.
    reader.join();
    CloseHandle(hRead);

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

// -------------------------------------------------------------
// hr_check_ffmpeg
// -------------------------------------------------------------
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

// -------------------------------------------------------------
// hr_get_dshow_devices
// -------------------------------------------------------------
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

// -------------------------------------------------------------
// hr_probe_gpu  (run on a background thread)
// -------------------------------------------------------------
// The probe clip's size has to be at least as big as the smallest
// resolution these hardware encoders will actually accept - 32x32 (the
// previous size here) is below the minimum coded size several NVENC/QSV/
// AMF driver versions enforce, so the probe would report "failed" even on
// machines where the encoder works perfectly fine at real capture
// resolutions. That false negative silently forced every recording onto
// software libx264 (much heavier on the CPU) despite a working GPU
// encoder sitting right there. 1280x720 is comfortably above every known
// per-vendor minimum while still encoding a 0.1s clip near-instantly.
struct GpuCand { const wchar_t* name; const wchar_t* extra_args; };
static const GpuCand k_gpu[] = {
    { L"h264_nvenc", L" -f lavfi -i nullsrc=s=1280x720:d=0.1 -c:v h264_nvenc -f null -" },
    { L"h264_amf",   L" -f lavfi -i nullsrc=s=1280x720:d=0.1 -c:v h264_amf   -f null -" },
    { L"h264_qsv",   L" -f lavfi -i nullsrc=s=1280x720:d=0.1 -c:v h264_qsv   -f null -" },
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

// -------------------------------------------------------------
// hr_build_codec_args
// Returns space-separated ffmpeg argument string in out_buf.
// -------------------------------------------------------------
// hr_build_codec_args_ex - same as hr_build_codec_args() plus the real encode
// size (w x h, 0 = unknown -> assumed 1920x1080). Needed because Intel QSV is
// now driven by a bitrate derived from resolution*fps*quality (see below).
HR_EXPORT int hr_build_codec_args_ex(const wchar_t* codec,
                                      int quality, int fps, int cpu_count,
                                      int enc_w, int enc_h,
                                      wchar_t* out_buf, int buf_chars,
                                      const wchar_t* preset_override)
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
        // BUGFIX: this used to pass "-qp N". h264_qsv has NO "qp" option
        // (ffmpeg only prints "Codec AVOption qp ... has not been used for
        // any stream" and carries on), so the Quality slider did nothing for
        // Intel QSV and the encoder silently ran at ffmpeg's built-in QSV
        // default (~1 Mbps). QSV needs a bitrate (or -global_quality, which
        // requires ICQ support = Skylake or newer and would refuse to start
        // on older iGPUs). Use plain VBR with a cap: works on every QSV
        // generation, gives a predictable encoder load/file size (like OBS's
        // fixed-bitrate QSV) and no runaway bitrate spikes on busy scenes.
        //   bits-per-pixel-per-frame: 0.04 (quality 0) .. 0.18 (quality 100)
        //   1080p30: q=30 -> ~5.1 Mbps (the OBS setting), q=50 -> ~6.8,
        //            q=100 -> ~11 Mbps.
        int w = enc_w > 0 ? enc_w : 1920;
        int h = enc_h > 0 ? enc_h : 1080;
        int q = quality < 0 ? 0 : (quality > 100 ? 100 : quality);
        double bpp = 0.04 + 0.14 * (q / 100.0);
        long long kbps = (long long)((double)w * h * (fps > 0 ? fps : 30) * bpp / 1000.0);
        if (kbps < 1500)  kbps = 1500;
        if (kbps > 60000) kbps = 60000;
        // -low_power is deliberately NOT forced: it needs VDENC hardware that
        // older/lower-end Intel iGPUs don't have and ffmpeg would just fail
        // to init QSV on those; -bf 0 avoids B-frame reordering delay and
        // work, -look_ahead 0 keeps the encoder single-pass.
        ss << L" -preset veryfast -look_ahead 0 -bf 0 -b:v " << kbps << L"k -maxrate "
           << (kbps * 3 / 2) << L"k -bufsize " << (kbps * 2) << L"k -g " << gop;
    } else if (is_amf) {
        ss << L" -quality speed -rc cqp -qp_i " << qp << L" -qp_p " << qp << L" -g " << gop;
    } else {
        // This used to do the opposite of what you'd want here --
        // 1 thread flat on anything with 4 cores or fewer, and only
        // cpu_count/4 above that (so even an 8-core machine got just 2
        // x264 threads, a 16-core one only 4). libx264's "ultrafast"
        // preset is cheap per-thread but still needs to encode every
        // frame in real time; forcing that through one or two threads
        // means one or two cores end up pegged doing the whole job while
        // the rest of the machine sits idle, which is exactly what
        // "the fan spins up, Task Manager shows 50%+" looks like on a
        // quad-core+ machine (100% of one core alone is 25%+ of a
        // 4-core/8-thread CPU's total). x264 parallelizes across frames
        // efficiently at low presets, so give it most of the cores
        // instead, just holding a couple back for the capture thread
        // (DXGI grab + overlay compositing + BGRA->YUV) and everything
        // else (audio, UI) to run on without contention.
        int thr = std::max(1, cpu_count - 2);
        // The "Encoder preset" dropdown on the Video/Codec settings
        // tab (Settings > Video > Encoder preset - ultrafast..veryslow,
        // AppState::enc_preset) was saved, reloaded, round-tripped through
        // every settings-persistence layer... and never actually read at
        // recording time. This function always hardcoded "ultrafast"
        // regardless of what the user picked, so choosing e.g. "medium"
        // for better quality silently did nothing. Now honors the
        // caller-supplied override (falls back to "ultrafast" if empty/
        // null, so behavior for anyone who's never touched that dropdown
        // is unchanged).
        std::wstring preset = (preset_override && preset_override[0]) ? preset_override : L"ultrafast";
        ss << L" -preset " << preset << L" -crf " << qp
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

HR_EXPORT int hr_build_codec_args(const wchar_t* codec,
                                   int quality, int fps, int cpu_count,
                                   wchar_t* out_buf, int buf_chars,
                                   const wchar_t* preset_override)
{
    return hr_build_codec_args_ex(codec, quality, fps, cpu_count, 0, 0,
                                  out_buf, buf_chars, preset_override);
}

// -------------------------------------------------------------
// probe_duration_sec
// Runs "ffmpeg -i <path>" with no output and reads the
// "Duration: HH:MM:SS.xx" line ffmpeg prints for any input on
// stderr - the standard way to get a file's duration without
// bundling ffprobe separately (this project only ships
// ffmpeg.exe). Returns -1.0 if the file couldn't be probed or no
// Duration line was found (e.g. "N/A" for a still-being-written
// or malformed file).
// -------------------------------------------------------------
static double probe_duration_sec(const std::wstring& ffpath, const std::wstring& path)
{
    std::wstring out = run_cmd(L"\"" + ffpath + L"\" -i \"" + path + L"\"", 6000);
    size_t pos = out.find(L"Duration: ");
    if (pos == std::wstring::npos) return -1.0;
    pos += 10; // skip "Duration: "
    int h = 0, m = 0; double s = 0.0;
    if (swscanf_s(out.c_str() + pos, L"%d:%d:%lf", &h, &m, &s) != 3) return -1.0;
    return h * 3600.0 + m * 60.0 + s;
}

// -------------------------------------------------------------
// hr_merge_av
// -------------------------------------------------------------
HR_EXPORT int hr_merge_av(const wchar_t* ffpath,
                           const wchar_t* video_file,
                           const wchar_t* audio_file,
                           double real_elapsed_sec,
                           double av_start_skew_sec)
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

    double video_dur = probe_duration_sec(ffpath, vf);
    bool stretch = real_elapsed_sec > 0.5 && video_dur > 0.05 &&
                   video_dur < real_elapsed_sec * 0.9;

    std::wstring audio_pre;   // goes right before "-i audio_file"
    if (av_start_skew_sec > 0.02) {
        wchar_t buf[64];
        swprintf_s(buf, L"%.6f", av_start_skew_sec);
        audio_pre = L" -itsoffset " + std::wstring(buf);
    } else if (av_start_skew_sec < -0.02) {
        wchar_t buf[64];
        swprintf_s(buf, L"%.6f", -av_start_skew_sec);
        audio_pre = L" -ss " + std::wstring(buf);
    }

    std::wstring cmd;
    if (stretch) {
        double ratio = real_elapsed_sec / video_dur;
        wchar_t ratio_buf[64];
        swprintf_s(ratio_buf, L"%.6f", ratio);
        HrLog::Warn("Recording: the encoded video came out much shorter than the "
                    "real recording time (likely dropped frames from an overloaded "
                    "machine) - stretching it back to real speed before merging "
                    "audio, instead of cutting the audio down to match.");
        cmd =
            L"\"" + std::wstring(ffpath) + L"\""
            L" -i \"" + vf + L"\""
            + audio_pre +
            L" -i \"" + std::wstring(audio_file) + L"\""
            L" -vf \"setpts=" + std::wstring(ratio_buf) + L"*PTS\""
            L" -c:v libx264 -preset veryfast -crf 20 -c:a aac"
            L" -af aresample=async=1000"
            L" -map 0:v:0 -map 1:a:0"
            L" -shortest -y"
            L" \"" + tmp + L"\"";
    } else {
        cmd =
            L"\"" + std::wstring(ffpath) + L"\""
            L" -i \"" + vf + L"\""
            + audio_pre +
            L" -i \"" + std::wstring(audio_file) + L"\""
            L" -c:v copy -c:a aac"
            L" -af aresample=async=1000"
            L" -map 0:v:0 -map 1:a:0"
            L" -shortest -y"
            L" \"" + tmp + L"\"";
    }

    run_cmd(cmd, 180000);

    if (!fexists(tmp)) return 0;

    DeleteFileW(video_file);
    if (!MoveFileW(tmp.c_str(), video_file)) {
        CopyFileW(tmp.c_str(), video_file, FALSE);
        DeleteFileW(tmp.c_str());
    }
    return fexists(vf) ? 1 : 0;
}

// -------------------------------------------------------------
// hr_export_mp3
// Encodes a WAV file to MP3 (libmp3lame) -- backs the Settings > Advanced
// "Also save audio as a separate MP3" checkbox (state_.separate_audio_mp3),
// which was previously persisted/shown in the UI but never actually acted
// on anywhere (see recording_controller.cpp's Stop()).
// -------------------------------------------------------------
HR_EXPORT int hr_export_mp3(const wchar_t* ffpath, const wchar_t* wav_path, const wchar_t* mp3_path)
{
    if (!ffpath || !wav_path || !mp3_path) return 0;

    std::wstring cmd =
        L"\"" + std::wstring(ffpath) + L"\""
        L" -i \"" + std::wstring(wav_path) + L"\""
        L" -c:a libmp3lame -q:a 2 -y"
        L" \"" + std::wstring(mp3_path) + L"\"";

    run_cmd(cmd, 60000);
    return fexists(mp3_path) ? 1 : 0;
}

HR_EXPORT int hr_concat_segments(const wchar_t* ffpath, const wchar_t* list_path, const wchar_t* out_path)
{
    if (!ffpath || !list_path || !out_path) return 0;

    std::wstring cmd =
        L"\"" + std::wstring(ffpath) + L"\""
        L" -f concat -safe 0 -i \"" + std::wstring(list_path) + L"\""
        L" -c copy -y"
        L" \"" + std::wstring(out_path) + L"\"";

    run_cmd(cmd, 60000);
    return fexists(out_path) ? 1 : 0;
}
