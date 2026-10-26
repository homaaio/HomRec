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
#include "../utils/log.h"

#ifdef _WIN32
  #define HR_EXPORT extern "C" __declspec(dllexport)
#else
  #define HR_EXPORT extern "C" __attribute__((visibility("default")))
#endif

// Internal: run a command, return combined stdout+stderr output
struct RunResult {
    bool  started   = false;  // CreateProcessW succeeded
    bool  timed_out = false;  // killed because timeout_ms elapsed
    DWORD exit_code = 1;      // valid only when started && !timed_out
    bool ok() const { return started && !timed_out && exit_code == 0; }
};

static std::wstring run_cmd_ex(const std::wstring& cmd, DWORD timeout_ms, RunResult* rr)
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
                        CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, nullptr, nullptr, &si, &pi))
    {
        CloseHandle(hRead); CloseHandle(hWrite);
        return {};
    }
    CloseHandle(hWrite);
    if (rr) rr->started = true;

    std::string raw;
    std::thread reader([&]() {
        char buf[4096]; DWORD br = 0;
        while (ReadFile(hRead, buf, sizeof(buf) - 1, &br, nullptr) && br) {
            buf[br] = '\0';
            raw += buf;
        }
    });

    const DWORD wait_res = WaitForSingleObject(pi.hProcess, timeout_ms);
    if (rr) {
        rr->timed_out = (wait_res == WAIT_TIMEOUT);
        DWORD ec = 1;
        // Read the exit code BEFORE TerminateProcess() below - that would overwrite it.
        if (!rr->timed_out && GetExitCodeProcess(pi.hProcess, &ec)) rr->exit_code = ec;
    }
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

static std::wstring run_cmd(const std::wstring& cmd, DWORD timeout_ms = 8000)
{
    return run_cmd_ex(cmd, timeout_ms, nullptr);
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

// hr_probe_gpu  (run on a background thread)
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

// hr_probe_ddagrab 1 if this ffmpeg build has the "ddagrab" source filter (ffmpeg 6.0+
// with D3D11 support): DXGI desktop capture whose frames stay on the GPU.
HR_EXPORT int hr_probe_ddagrab(const wchar_t* ffpath)
{
    if (!ffpath || !*ffpath) return 0;
    std::wstring cmd = L"\"" + std::wstring(ffpath) + L"\" -hide_banner -filters";
    std::wstring res = run_cmd(cmd, 8000);
    return res.find(L"ddagrab") != std::wstring::npos ? 1 : 0;
}

// hr_build_codec_args
// Returns space-separated ffmpeg argument string in out_buf.
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
        // This used to pass "-qp N".
        int w = enc_w > 0 ? enc_w : 1920;
        int h = enc_h > 0 ? enc_h : 1080;
        int q = quality < 0 ? 0 : (quality > 100 ? 100 : quality);
        double bpp = 0.04 + 0.14 * (q / 100.0);
        long long kbps = (long long)((double)w * h * (fps > 0 ? fps : 30) * bpp / 1000.0);
        if (kbps < 1500)  kbps = 1500;
        if (kbps > 60000) kbps = 60000;
        ss << L" -preset veryfast -look_ahead 0 -bf 0 -b:v " << kbps << L"k -maxrate "
           << (kbps * 3 / 2) << L"k -bufsize " << (kbps * 2) << L"k -g " << gop;
    } else if (is_amf) {
        ss << L" -quality speed -rc cqp -qp_i " << qp << L" -qp_p " << qp << L" -g " << gop;
    } else {
        // This used to do the opposite of what you'd want here -- 1 thread flat on anything with 4
        // cores or fewer, and only cpu_count/4 above that (so even an 8-core machine got just 2
        // x264 threads, a 16-core one only 4).
        int thr = std::max(1, cpu_count - 2);
        // The "Encoder preset" dropdown on the Video/Codec settings tab (Settings > Video >
        // Encoder preset - ultrafast..veryslow, AppState::enc_preset) was saved, reloaded, round-
        // tripped through every settings-persistence layer...
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
// hr_merge_av / hr_merge_av_ex
// -------------------------------------------------------------
// hr_merge_av_ex() = hr_merge_av() + optionally writing the .mp4 copy of "Both" recordings in the SAME
// ffmpeg pass (one read of the video instead of two, no separate remux afterwards - the second full
// read+write of a long recording was a big part of the multi-second "saving" wait after Stop).
// mp4_copy_path may be null; *mp4_done tells the caller whether that copy now exists.
HR_EXPORT int hr_merge_av_ex(const wchar_t* ffpath,
                              const wchar_t* video_file,
                              const wchar_t* audio_file,
                              double real_elapsed_sec,
                              double av_start_skew_sec,
                              const wchar_t* mp4_copy_path,
                              int* mp4_done)
{
    if (mp4_done) *mp4_done = 0;
    if (!ffpath || !video_file || !audio_file) return 0;

    std::wstring vf(video_file);
    // Build temp output path
    std::wstring tmp = vf;
    size_t dot = tmp.rfind(L'.');
    if (dot != std::wstring::npos)
        tmp = tmp.substr(0, dot) + L"_mrgtmp" + tmp.substr(dot);
    else
        tmp += L"_mrgtmp.mp4";

    std::wstring tmp_mp4;
    if (mp4_copy_path && *mp4_copy_path) {
        tmp_mp4 = mp4_copy_path;
        size_t d2 = tmp_mp4.rfind(L'.');
        tmp_mp4 = (d2 == std::wstring::npos) ? tmp_mp4 + L"_rmxtmp.mp4"
                                              : tmp_mp4.substr(0, d2) + L"_rmxtmp" + tmp_mp4.substr(d2);
    }

    double video_dur = probe_duration_sec(ffpath, vf);
    double audio_dur = probe_duration_sec(ffpath, std::wstring(audio_file));

    // A video that is only a little shorter than the real recording time (a few dropped frames during
    // a CPU spike) used to be left alone - the old rule only retimed when it was >10% short - and the
    // audio, which follows the wall clock, then ran late against the picture by exactly that much.
    const bool have_real = real_elapsed_sec > 0.5 && video_dur > 0.05;
    bool stretch = have_real && video_dur < real_elapsed_sec * 0.9995 && (real_elapsed_sec - video_dur) > 0.4;

    if (have_real || audio_dur > 0.0) {
        wchar_t lb[160];
        swprintf_s(lb, L"%.3f / %.3f / %.3f", video_dur, audio_dur, real_elapsed_sec);
        std::wstring w = lb;
        std::string n(w.begin(), w.end());   // ASCII digits only
        HrLog::Info("Recording: lengths before merge (video / audio / real, seconds) = " + n);
    }

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

    // (The audio track is kept on the wall clock while it is captured - see WasapiStream::read() - and its
    // tail legitimately runs a moment past the Stop click, so it is NOT tempo-corrected here. The line
    // logged above is what to look at if a recording ever still drifts.)
    const std::wstring af = L" -af aresample=async=1000";

    wchar_t ratio_buf[64] = L"1";
    const double stretch_ratio = (stretch && video_dur > 0.0) ? real_elapsed_sec / video_dur : 1.0;
    if (stretch) {
        swprintf_s(ratio_buf, L"%.6f", stretch_ratio);
        HrLog::Warn("Recording: the encoded video came out shorter than the "
                    "real recording time (likely dropped frames from an overloaded "
                    "machine) - stretching it back to real speed before merging "
                    "audio, instead of cutting the audio down to match.");
    }

    const std::wstring inputs =
        L"\"" + std::wstring(ffpath) + L"\""
        L" -i \"" + vf + L"\""
        + audio_pre +
        L" -i \"" + std::wstring(audio_file) + L"\"";

    // One output = stream-copied video + AAC audio. With `with_setts` the video timestamps are
    // rescaled by the copy-only "setts" bitstream filter (fast: no re-encode).
    auto output_part = [&](const std::wstring &out, bool faststart, bool with_setts) {
        std::wstring o = L" -map 0:v:0 -map 1:a:0";
        if (with_setts)
            o += L" -bsf:v \"setts=ts=TS*" + std::wstring(ratio_buf) + L":duration=DURATION*" + std::wstring(ratio_buf) + L"\"";
        o += L" -c:v copy -c:a aac" + af + L" -shortest";
        if (faststart) o += L" -movflags +faststart";
        o += L" -y \"" + out + L"\"";
        return o;
    };

    // The retime used to RE-ENCODE the whole video with libx264 -- on the very machine that was already
    // too overloaded to keep up while recording. Copy-retime is tried first; this is only the fallback.
    const std::wstring cmd_reencode =
        inputs +
        L" -vf \"setpts=" + std::wstring(ratio_buf) + L"*PTS\""
        L" -c:v libx264 -preset veryfast -crf 20 -c:a aac" + af +
        L" -map 0:v:0 -map 1:a:0 -shortest -y \"" + tmp + L"\"";

    // The fixed 180 s timeout killed ffmpeg mid-write on long recordings (the stretch path re-
    // encodes the WHOLE video), and the code below then replaced the finished recording with
    // that truncated temp file - permanent data loss.
    double timeout_sec = 180.0;
    if (video_dur > 0.0) {
        const double want = video_dur * 0.5;   // stream copy: seconds, not minutes
        if (want > timeout_sec) timeout_sec = want;
    }
    if (timeout_sec > 6.0 * 3600.0) timeout_sec = 6.0 * 3600.0;

    RunResult rr;
    bool mp4_in_pass = !tmp_mp4.empty();
    {
        std::wstring cmd = inputs + output_part(tmp, false, stretch);
        if (mp4_in_pass) cmd += output_part(tmp_mp4, true, stretch);
        run_cmd_ex(cmd, (DWORD)((mp4_in_pass ? timeout_sec * 2.0 : timeout_sec) * 1000.0), &rr);
    }
    // A problem with the extra .mp4 output must never cost the .mkv its audio: redo the merge without it
    // (the caller then falls back to the separate remux).
    if (mp4_in_pass && (!rr.ok() || !fexists(tmp) || !fexists(tmp_mp4))) {
        HrLog::Warn("Recording: writing the .mkv and the .mp4 in one pass failed - redoing the merge for the .mkv alone.");
        DeleteFileW(tmp.c_str());
        DeleteFileW(tmp_mp4.c_str());
        mp4_in_pass = false;
        rr = RunResult{};
        run_cmd_ex(inputs + output_part(tmp, false, stretch), (DWORD)(timeout_sec * 1000.0), &rr);
    }

    // Stretch path only: if the copy-retime was refused (an old ffmpeg without "setts", an
    // odd container...) fall back to the original full re-encode so the recording still ends
    // up with correct timing -- that is the only case where the heavy path runs now. A tiny
    // correction (<2%) isn't worth a full re-encode: merge without the retime instead.
    if (stretch && (!rr.ok() || !fexists(tmp))) {
        DeleteFileW(tmp.c_str());
        if (stretch_ratio < 1.02) {
            HrLog::Warn("Recording: fast timestamp retime wasn't accepted by ffmpeg - merging without it (the correction was under 2%).");
            rr = RunResult{};
            run_cmd_ex(inputs + output_part(tmp, false, false), (DWORD)(timeout_sec * 1000.0), &rr);
        } else {
            HrLog::Warn("Recording: fast timestamp retime wasn't accepted by ffmpeg - falling back to a full re-encode.");
            double re_timeout = 180.0;
            if (video_dur > 0.0 && video_dur * 2.0 > re_timeout) re_timeout = video_dur * 2.0;
            if (re_timeout > 6.0 * 3600.0) re_timeout = 6.0 * 3600.0;
            rr = RunResult{};
            run_cmd_ex(cmd_reencode, (DWORD)(re_timeout * 1000.0), &rr);
        }
        mp4_in_pass = false;   // the .mp4 written alongside (if any) was made by the failed command
        if (!tmp_mp4.empty()) DeleteFileW(tmp_mp4.c_str());
    }

    if (!fexists(tmp)) { if (!tmp_mp4.empty()) DeleteFileW(tmp_mp4.c_str()); return 0; }
    if (!rr.ok()) {
        HrLog::Error(rr.timed_out
            ? "Recording: merging audio into the video timed out - keeping the video without audio "
              "(the separate audio file is kept so nothing is lost)."
            : "Recording: ffmpeg reported an error while merging audio - keeping the video without audio "
              "(the separate audio file is kept so nothing is lost).");
        DeleteFileW(tmp.c_str());
        if (!tmp_mp4.empty()) DeleteFileW(tmp_mp4.c_str());
        return 0;
    }

    // Replace in one step - the old delete-then-move left NO video at all if the
    // move failed.
    if (!MoveFileExW(tmp.c_str(), video_file, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
        HrLog::Error("Recording: couldn't replace the video with the merged file - keeping the original.");
        DeleteFileW(tmp.c_str());
        if (!tmp_mp4.empty()) DeleteFileW(tmp_mp4.c_str());
        return 0;
    }
    if (mp4_in_pass && fexists(tmp_mp4)) {
        if (MoveFileExW(tmp_mp4.c_str(), mp4_copy_path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
            if (mp4_done) *mp4_done = 1;
        } else {
            DeleteFileW(tmp_mp4.c_str());
        }
    } else if (!tmp_mp4.empty()) {
        DeleteFileW(tmp_mp4.c_str());
    }
    return fexists(vf) ? 1 : 0;
}

HR_EXPORT int hr_merge_av(const wchar_t* ffpath,
                           const wchar_t* video_file,
                           const wchar_t* audio_file,
                           double real_elapsed_sec,
                           double av_start_skew_sec)
{
    return hr_merge_av_ex(ffpath, video_file, audio_file, real_elapsed_sec, av_start_skew_sec, nullptr, nullptr);
}

HR_EXPORT int hr_export_mp3(const wchar_t* ffpath, const wchar_t* wav_path, const wchar_t* mp3_path)
{
    if (!ffpath || !wav_path || !mp3_path) return 0;

    std::wstring cmd =
        L"\"" + std::wstring(ffpath) + L"\""
        L" -i \"" + std::wstring(wav_path) + L"\""
        L" -vn -c:a libmp3lame -q:a 2 -y"
        L" \"" + std::wstring(mp3_path) + L"\"";

    RunResult rr;
    run_cmd_ex(cmd, 120000, &rr);
    if (!rr.ok()) { DeleteFileW(mp3_path); return 0; } // never leave a half-written .mp3 behind
    return fexists(mp3_path) ? 1 : 0;
}

// hr_remux_copy  (2.4)
// Stream-copies every stream of in_path into out_path (no re-encode, so it
// costs almost nothing). Used for Settings > Video > Container "mp4 + mkv":
HR_EXPORT int hr_remux_copy(const wchar_t* ffpath, const wchar_t* in_path, const wchar_t* out_path)
{
    if (!ffpath || !in_path || !out_path) return 0;

    std::wstring out(out_path);
    // Write to a temp name first so a half-written .mp4 is never left behind.
    std::wstring tmp = out;
    size_t dot = tmp.rfind(L'.');
    tmp = (dot == std::wstring::npos) ? tmp + L"_rmxtmp.mp4" : tmp.substr(0, dot) + L"_rmxtmp" + tmp.substr(dot);

    std::wstring cmd =
        L"\"" + std::wstring(ffpath) + L"\""
        L" -i \"" + std::wstring(in_path) + L"\""
        L" -map 0 -c copy -movflags +faststart -y"
        L" \"" + tmp + L"\"";

    RunResult rr;
    run_cmd_ex(cmd, 10 * 60 * 1000, &rr);
    if (!rr.ok() || !fexists(tmp)) { DeleteFileW(tmp.c_str()); return 0; }
    if (!MoveFileExW(tmp.c_str(), out_path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
        DeleteFileW(tmp.c_str());
        return 0;
    }
    return fexists(out) ? 1 : 0;
}

// hr_decode_audio_to_wav  (2.4)
HR_EXPORT int hr_decode_audio_to_wav(const wchar_t* ffpath, const wchar_t* in_path, const wchar_t* wav_path)
{
    if (!ffpath || !in_path || !wav_path) return 0;
    std::wstring cmd =
        L"\"" + std::wstring(ffpath) + L"\""
        L" -i \"" + std::wstring(in_path) + L"\""
        L" -vn -ac 2 -ar 44100 -c:a pcm_s16le -y"
        L" \"" + std::wstring(wav_path) + L"\"";
    RunResult rr;
    run_cmd_ex(cmd, 5 * 60 * 1000, &rr);
    if (!rr.ok()) { DeleteFileW(wav_path); return 0; }
    return fexists(wav_path) ? 1 : 0;
}

HR_EXPORT int hr_concat_segments(const wchar_t* ffpath, const wchar_t* list_path, const wchar_t* out_path)
{
    if (!ffpath || !list_path || !out_path) return 0;

    std::wstring cmd =
        L"\"" + std::wstring(ffpath) + L"\""
        L" -f concat -safe 0 -i \"" + std::wstring(list_path) + L"\""
        L" -c copy -y"
        L" \"" + std::wstring(out_path) + L"\"";

    RunResult rr;
    run_cmd_ex(cmd, 180000, &rr);
    if (!rr.ok()) { DeleteFileW(out_path); return 0; } // never present a truncated clip as a saved replay
    return fexists(out_path) ? 1 : 0;
}
