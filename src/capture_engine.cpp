#include "capture_engine.hpp"
#include <cmath>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmsystem.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <algorithm>

#pragma comment(lib, "ole32.lib")

// -------------------- helpers ----------------------------------------------
static std::string WstrToUtf8(const wchar_t* ws) {
    if (!ws) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, ws, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws, -1, &s[0], n, nullptr, nullptr);
    if (!s.empty() && s.back() == '\0') s.pop_back();
    return s;
}
static std::wstring Utf8ToWstr(const char* u) {
    if (!u) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, u, -1, nullptr, 0);
    std::wstring ws(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, u, -1, &ws[0], n);
    if (!ws.empty() && ws.back() == L'\0') ws.pop_back();
    return ws;
}

// -------------------- TIMER -----------------------------------------------
extern "C" void HR_SetTimerResolution(int on) {
    if (on) timeBeginPeriod(1);
    else    timeEndPeriod(1);
}
extern "C" int HR_GetVersion(char* buf, int buf_size) {
    const char* v = "1.4.4 (Legacy)";
    strncpy(buf, v, (size_t)buf_size - 1);
    buf[buf_size - 1] = '\0';
    return 0;
}

// -------------------- MONITOR ENUM ----------------------------------------
struct MonRect { int x, y, w, h; };
static std::vector<MonRect> g_monitors;
static BOOL CALLBACK MonitorEnumProc(HMONITOR, HDC, LPRECT rc, LPARAM) {
    MonRect m{ rc->left, rc->top, rc->right - rc->left, rc->bottom - rc->top };
    g_monitors.push_back(m);
    return TRUE;
}
extern "C" int HR_GetMonitorCount() {
    g_monitors.clear();
    EnumDisplayMonitors(nullptr, nullptr, MonitorEnumProc, 0);
    return (int)g_monitors.size();
}
extern "C" int HR_GetMonitorRect(int idx, int* x, int* y, int* w, int* h) {
    if (idx < 0 || idx >= (int)g_monitors.size()) return -1;
    *x = g_monitors[idx].x; *y = g_monitors[idx].y;
    *w = g_monitors[idx].w; *h = g_monitors[idx].h;
    return 0;
}

// -------------------- FAST BILINEAR SCALE (RGB24) -------------------------
// Optimised for downscaling preview frames on weak CPUs.
// Uses integer arithmetic only — no floating point.
static void ScaleRGB24(
    const uint8_t* src, int sw, int sh,
    uint8_t* dst,       int dw, int dh)
{
    // x/y step in src-space (fixed-point 16.16)
    const int xstep = (sw << 16) / dw;
    const int ystep = (sh << 16) / dh;
    int fy = 0;
    for (int dy = 0; dy < dh; dy++, fy += ystep) {
        int sy0 = fy >> 16;
        int sy1 = (sy0 + 1 < sh) ? sy0 + 1 : sh - 1;
        int wy1 = (fy & 0xFFFF) >> 8;  // 0-255
        int wy0 = 256 - wy1;
        const uint8_t* row0 = src + sy0 * sw * 3;
        const uint8_t* row1 = src + sy1 * sw * 3;
        uint8_t* drow = dst + dy * dw * 3;
        int fx = 0;
        for (int dx = 0; dx < dw; dx++, fx += xstep) {
            int sx0 = fx >> 16;
            int sx1 = (sx0 + 1 < sw) ? sx0 + 1 : sw - 1;
            int wx1 = (fx & 0xFFFF) >> 8;
            int wx0 = 256 - wx1;
            for (int c = 0; c < 3; c++) {
                int v = (row0[sx0*3+c]*wx0 + row0[sx1*3+c]*wx1) * wy0
                      + (row1[sx0*3+c]*wx0 + row1[sx1*3+c]*wx1) * wy1;
                drow[dx*3+c] = (uint8_t)(v >> 16);
            }
        }
    }
}

// -------------------- SCREEN CAPTURE (GDI BitBlt) -------------------------
// Single-shot capture — used both by preview loop and recording fallback.
// Returns raw RGB24 of the captured & scaled region.
extern "C" int HR_CaptureScreen(
    int cx, int cy, int cw, int ch,
    uint8_t* out_rgb24,
    int out_w, int out_h)
{
    HDC hdc_screen = GetDC(nullptr);
    if (!hdc_screen) return -1;

    HDC hdc_mem = CreateCompatibleDC(hdc_screen);
    HBITMAP hbm = CreateCompatibleBitmap(hdc_screen, cw, ch);
    HGDIOBJ old = SelectObject(hdc_mem, hbm);

    // Use SRCCOPY — fastest path on all Windows versions
    BitBlt(hdc_mem, 0, 0, cw, ch, hdc_screen, cx, cy, SRCCOPY);

    // Extract BGRX pixels
    BITMAPINFOHEADER bi{};
    bi.biSize        = sizeof(bi);
    bi.biWidth       = cw;
    bi.biHeight      = -ch;   // top-down
    bi.biPlanes      = 1;
    bi.biBitCount    = 32;
    bi.biCompression = BI_RGB;

    std::vector<uint8_t> bgrx(cw * ch * 4);
    GetDIBits(hdc_mem, hbm, 0, ch, bgrx.data(),
              (BITMAPINFO*)&bi, DIB_RGB_COLORS);

    SelectObject(hdc_mem, old);
    DeleteObject(hbm);
    DeleteDC(hdc_mem);
    ReleaseDC(nullptr, hdc_screen);

    // Convert BGRX → RGB24
    std::vector<uint8_t> rgb24(cw * ch * 3);
    for (int i = 0; i < cw * ch; i++) {
        rgb24[i*3+0] = bgrx[i*4+2]; // R
        rgb24[i*3+1] = bgrx[i*4+1]; // G
        rgb24[i*3+2] = bgrx[i*4+0]; // B
    }

    if (out_w == cw && out_h == ch) {
        memcpy(out_rgb24, rgb24.data(), cw * ch * 3);
    } else {
        ScaleRGB24(rgb24.data(), cw, ch, out_rgb24, out_w, out_h);
    }
    return out_w * out_h * 3;
}

// -------------------- PREVIEW THREAD --------------------------------------
struct PreviewState {
    std::atomic<bool>  running{false};
    std::thread        thr;
    std::mutex         mtx;
    std::vector<uint8_t> buf;
    int  pw{0}, ph{0};
    std::atomic<bool>  fresh{false};

    int cx, cy, cw, ch, fps;
};
static PreviewState g_prev;

static void PreviewLoop(int cx, int cy, int cw, int ch, int pw, int ph, int fps) {
    DWORD interval_ms = (fps > 0) ? (1000u / fps) : 50;
    std::vector<uint8_t> tmp(pw * ph * 3);

    while (g_prev.running.load()) {
        DWORD t0 = timeGetTime();
        HR_CaptureScreen(cx, cy, cw, ch, tmp.data(), pw, ph);
        {
            std::lock_guard<std::mutex> lk(g_prev.mtx);
            memcpy(g_prev.buf.data(), tmp.data(), tmp.size());
            g_prev.fresh.store(true);
        }
        DWORD elapsed = timeGetTime() - t0;
        if (elapsed < interval_ms)
            Sleep(interval_ms - elapsed);
    }
}

extern "C" int HR_PreviewStart(int cx, int cy, int cw, int ch,
                                int pw, int ph, int fps_limit) {
    if (g_prev.running.load()) HR_PreviewStop();
    g_prev.pw = pw; g_prev.ph = ph;
    g_prev.buf.resize(pw * ph * 3, 0);
    g_prev.running.store(true);
    g_prev.thr = std::thread(PreviewLoop, cx, cy, cw, ch, pw, ph, fps_limit);
    return 0;
}
extern "C" void HR_PreviewStop() {
    g_prev.running.store(false);
    if (g_prev.thr.joinable()) g_prev.thr.join();
}
extern "C" int HR_PreviewGetFrame(uint8_t* dst, int* out_w, int* out_h) {
    if (!g_prev.fresh.load()) return 0;
    std::lock_guard<std::mutex> lk(g_prev.mtx);
    memcpy(dst, g_prev.buf.data(), g_prev.buf.size());
    *out_w = g_prev.pw;
    *out_h = g_prev.ph;
    g_prev.fresh.store(false);
    return 1;
}

// -------------------- RECORDING (FFmpeg subprocess) -----------------------
struct RecState {
    std::atomic<bool>   running{false};
    std::atomic<int>    frames{0};
    HANDLE              proc_handle{nullptr};
    HANDLE              stdin_write{nullptr};
    std::thread         stderr_reader;
    std::atomic<bool>   stop_reader{false};
};
static RecState g_rec;

static std::string BuildFFmpegCmd(
    const char* ff, const char* out,
    int x, int y, int w, int h, int fps,
    const char* codec, const char* preset, int crf,
    const char* pix_fmt, const char* hw_accel,
    bool is_window, const char* window_title)
{
    std::string cmd = "\"";
    cmd += ff;
    cmd += "\" -y";

    // HW decode input (optional)
    if (hw_accel && strcmp(hw_accel,"none") != 0 && strcmp(hw_accel,"auto") != 0) {
        cmd += " -hwaccel "; cmd += hw_accel;
    }

    if (is_window && window_title && window_title[0]) {
        cmd += " -f gdigrab -framerate "; cmd += std::to_string(fps);
        cmd += " -i \"title="; cmd += window_title; cmd += "\"";
    } else {
        char buf[256];
        snprintf(buf, sizeof(buf),
            " -f gdigrab -framerate %d -offset_x %d -offset_y %d"
            " -video_size %dx%d -i desktop",
            fps, x, y, w, h);
        cmd += buf;
    }

    // Scale filter only when needed
    if (w > 1920) {
        cmd += " -vf scale=1920:-2";
    }

    char cbuf[512];
    snprintf(cbuf, sizeof(cbuf),
        " -r %d -c:v %s -preset %s -crf %d -pix_fmt %s"
        " -movflags +faststart -an",
        fps, codec, preset, crf, pix_fmt);
    cmd += cbuf;
    cmd += " \""; cmd += out; cmd += "\"";
    return cmd;
}

extern "C" int HR_RecordStart(
    const char* ffmpeg_path, const char* out_file,
    int x, int y, int w, int h, int fps,
    const char* codec, const char* preset, int crf,
    const char* pix_fmt, const char* hw_accel,
    int capture_window, const char* window_title)
{
    if (g_rec.running.load()) return -1;

    std::string cmd = BuildFFmpegCmd(
        ffmpeg_path, out_file, x, y, w, h, fps,
        codec, preset, crf, pix_fmt, hw_accel,
        capture_window != 0, window_title);

    SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE stdin_r{}, stdin_w{};
    HANDLE stderr_r{}, stderr_w{};
    CreatePipe(&stdin_r,  &stdin_w,  &sa, 0);
    CreatePipe(&stderr_r, &stderr_w, &sa, 4096);
    SetHandleInformation(stdin_w,  HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(stderr_r, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si{};
    si.cb          = sizeof(si);
    si.dwFlags     = STARTF_USESTDHANDLES;
    si.hStdInput   = stdin_r;
    si.hStdOutput  = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError   = stderr_w;

    PROCESS_INFORMATION pi{};
    std::vector<char> cmd_buf(cmd.begin(), cmd.end());
    cmd_buf.push_back('\0');

    BOOL ok = CreateProcessA(
        nullptr, cmd_buf.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);

    CloseHandle(stdin_r);
    CloseHandle(stderr_w);

    if (!ok) {
        CloseHandle(stdin_w);
        CloseHandle(stderr_r);
        return -2;
    }

    CloseHandle(pi.hThread);
    g_rec.proc_handle = pi.hProcess;
    g_rec.stdin_write = stdin_w;
    g_rec.frames.store(0);
    g_rec.running.store(true);
    g_rec.stop_reader.store(false);

    // Background thread: parse "frame=N" from ffmpeg stderr
    g_rec.stderr_reader = std::thread([stderr_r]() {
        char buf[256];
        std::string line;
        DWORD read;
        while (!g_rec.stop_reader.load()) {
            BOOL r = ReadFile(stderr_r, buf, sizeof(buf)-1, &read, nullptr);
            if (!r || read == 0) break;
            buf[read] = '\0';
            line += buf;
            // scan for "frame=  NNN"
            auto pos = line.find("frame=");
            if (pos != std::string::npos) {
                int n = 0;
                sscanf(line.c_str() + pos + 6, " %d", &n);
                if (n > 0) g_rec.frames.store(n);
                line.clear();
            } else if (line.size() > 2048) {
                line.clear();
            }
        }
        CloseHandle(stderr_r);
    });

    return 0;
}

extern "C" void HR_RecordStop() {
    if (!g_rec.running.load()) return;
    g_rec.running.store(false);
    g_rec.stop_reader.store(true);

    // Send 'q' to stdin to gracefully stop ffmpeg
    if (g_rec.stdin_write) {
        DWORD w;
        WriteFile(g_rec.stdin_write, "q", 1, &w, nullptr);
        CloseHandle(g_rec.stdin_write);
        g_rec.stdin_write = nullptr;
    }
    if (g_rec.proc_handle) {
        WaitForSingleObject(g_rec.proc_handle, 12000);
        CloseHandle(g_rec.proc_handle);
        g_rec.proc_handle = nullptr;
    }
    if (g_rec.stderr_reader.joinable())
        g_rec.stderr_reader.join();
}
extern "C" int HR_RecordIsRunning() {
    return g_rec.running.load() ? 1 : 0;
}
extern "C" int HR_RecordGetFrameCount() {
    return g_rec.frames.load();
}

// -------------------- AUDIO CAPTURE (WASAPI loopback + waveIn mic) --------
// Lightweight dual-channel capture → separate WAV files.
// Designed to run with minimal overhead on old hardware.

// ---- WAV file helpers ----
static void WriteWavHeader(FILE* f, int sample_rate, int channels, int bits, uint32_t data_size) {
    uint32_t byte_rate = sample_rate * channels * bits / 8;
    uint16_t block_align = channels * bits / 8;
    uint32_t chunk_size  = 36 + data_size;
    fwrite("RIFF", 1, 4, f);
    fwrite(&chunk_size, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f);
    uint32_t sub1 = 16; fwrite(&sub1, 4, 1, f);
    uint16_t audio_fmt = 1; fwrite(&audio_fmt, 2, 1, f);
    fwrite(&channels, 2, 1, f);
    fwrite(&sample_rate, 4, 1, f);
    fwrite(&byte_rate, 4, 1, f);
    fwrite(&block_align, 2, 1, f);
    fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f);
    fwrite(&data_size, 4, 1, f);
}
static void PatchWavDataSize(FILE* f) {
    long end = ftell(f);
    uint32_t data_size = (uint32_t)(end - 44);
    fseek(f, 40, SEEK_SET);
    fwrite(&data_size, 4, 1, f);
    uint32_t chunk_size = 36 + data_size;
    fseek(f, 4, SEEK_SET);
    fwrite(&chunk_size, 4, 1, f);
    fseek(f, end, SEEK_SET);
}

struct AudioState {
    std::atomic<bool> running{false};
    std::thread       mic_thr;
    std::thread       sys_thr;
    std::atomic<float> mic_level{0.f};
    std::atomic<float> sys_level{0.f};
    std::string mic_path, sys_path;
    int sample_rate, channels;
    float mic_vol, sys_vol;
};
static AudioState g_audio;

// ── MIC capture via waveIn (works on XP+) ──
static void MicThread(std::string path, int sr, int ch, float vol) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return;
    WriteWavHeader(f, sr, ch, 16, 0);

    WAVEFORMATEX wfx{};
    wfx.wFormatTag      = WAVE_FORMAT_PCM;
    wfx.nChannels       = (WORD)ch;
    wfx.nSamplesPerSec  = (DWORD)sr;
    wfx.wBitsPerSample  = 16;
    wfx.nBlockAlign     = wfx.nChannels * wfx.wBitsPerSample / 8;
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;

    HWAVEIN hwi = nullptr;
    if (waveInOpen(&hwi, WAVE_MAPPER, &wfx, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR)
    { fclose(f); return; }

    const int BUF_COUNT = 4;
    const int BUF_SIZE  = sr * ch * 2 / 4;  // 250ms each
    std::vector<std::vector<int16_t>> bufs(BUF_COUNT, std::vector<int16_t>(BUF_SIZE/2));
    std::vector<WAVEHDR> hdrs(BUF_COUNT);

    for (int i = 0; i < BUF_COUNT; i++) {
        ZeroMemory(&hdrs[i], sizeof(WAVEHDR));
        hdrs[i].lpData         = (LPSTR)bufs[i].data();
        hdrs[i].dwBufferLength = BUF_SIZE;
        waveInPrepareHeader(hwi, &hdrs[i], sizeof(WAVEHDR));
        waveInAddBuffer(hwi, &hdrs[i], sizeof(WAVEHDR));
    }
    waveInStart(hwi);

    int cur = 0;
    while (g_audio.running.load()) {
        while (!(hdrs[cur].dwFlags & WHDR_DONE)) {
            if (!g_audio.running.load()) goto done;
            Sleep(10);
        }
        // Apply volume & compute level
        {
            int n = hdrs[cur].dwBytesRecorded / 2;
            int16_t* p = bufs[cur].data();
            float sum = 0;
            for (int i = 0; i < n; i++) {
                int32_t s = (int32_t)(p[i] * vol);
                if (s >  32767) s =  32767;
                if (s < -32768) s = -32768;
                p[i] = (int16_t)s;
                sum += (float)(s * s);
            }
            float rms = (n > 0) ? sqrtf(sum / n) / 32768.f * 100.f : 0.f;
            g_audio.mic_level.store(rms);
            fwrite(p, 2, n, f);
        }
        hdrs[cur].dwFlags = 0;
        hdrs[cur].dwBytesRecorded = 0;
        waveInPrepareHeader(hwi, &hdrs[cur], sizeof(WAVEHDR));
        waveInAddBuffer(hwi, &hdrs[cur], sizeof(WAVEHDR));
        cur = (cur + 1) % BUF_COUNT;
    }
done:
    waveInStop(hwi);
    waveInReset(hwi);
    for (int i = 0; i < BUF_COUNT; i++)
        waveInUnprepareHeader(hwi, &hdrs[i], sizeof(WAVEHDR));
    waveInClose(hwi);
    PatchWavDataSize(f);
    fclose(f);
}

// ── SYSTEM AUDIO via WASAPI loopback (Win7+) ──
static void SysAudioThread(std::string path, int sr, int ch, float vol) {
    if (path.empty()) return;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    FILE* f = fopen(path.c_str(), "wb");
    if (!f) { CoUninitialize(); return; }
    WriteWavHeader(f, sr, ch, 16, 0);

    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice*           device     = nullptr;
    IAudioClient*        client     = nullptr;
    IAudioCaptureClient* capture    = nullptr;

    HRESULT hr;
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                          CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                          (void**)&enumerator);
    if (FAILED(hr)) goto cleanup;
    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    if (FAILED(hr)) goto cleanup;
    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&client);
    if (FAILED(hr)) goto cleanup;

    {
        WAVEFORMATEX* pwfx = nullptr;
        client->GetMixFormat(&pwfx);

        REFERENCE_TIME req = 200 * 10000LL;  // 200ms buffer
        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                AUDCLNT_STREAMFLAGS_LOOPBACK,
                                req, 0, pwfx, nullptr);
        if (FAILED(hr)) { CoTaskMemFree(pwfx); goto cleanup; }

        hr = client->GetService(__uuidof(IAudioCaptureClient), (void**)&capture);
        if (FAILED(hr)) { CoTaskMemFree(pwfx); goto cleanup; }
        client->Start();

        while (g_audio.running.load()) {
            Sleep(50);
            UINT32 pkt = 0;
            if (FAILED(capture->GetNextPacketSize(&pkt))) break;
            while (pkt > 0) {
                BYTE* data; UINT32 nframes; DWORD flags;
                if (FAILED(capture->GetBuffer(&data, &nframes, &flags, nullptr, nullptr))) break;

                // Convert to 16-bit PCM
                int n = nframes * pwfx->nChannels;
                float* fp = (float*)data;
                float sum = 0;
                for (UINT32 i = 0; i < nframes; i++) {
                    for (int c = 0; c < ch && c < (int)pwfx->nChannels; c++) {
                        float s = fp[i * pwfx->nChannels + c] * vol;
                        if (s >  1.f) s =  1.f;
                        if (s < -1.f) s = -1.f;
                        int16_t samp = (int16_t)(s * 32767.f);
                        fwrite(&samp, 2, 1, f);
                        sum += s * s;
                    }
                }
                float rms = (n > 0) ? sqrtf(sum / n) * 100.f : 0.f;
                g_audio.sys_level.store(rms);
                capture->ReleaseBuffer(nframes);
                capture->GetNextPacketSize(&pkt);
            }
        }
        client->Stop();
        CoTaskMemFree(pwfx);
    }

cleanup:
    if (capture)   { capture->Release(); }
    if (client)    { client->Release(); }
    if (device)    { device->Release(); }
    if (enumerator){ enumerator->Release(); }
    PatchWavDataSize(f);
    fclose(f);
    CoUninitialize();
}

extern "C" int HR_AudioStart(
    const char* out_mic, const char* out_sys,
    int sample_rate, int channels,
    float mic_vol, float sys_vol)
{
    if (g_audio.running.load()) return -1;
    g_audio.mic_path    = out_mic  ? out_mic  : "";
    g_audio.sys_path    = out_sys  ? out_sys  : "";
    g_audio.sample_rate = sample_rate;
    g_audio.channels    = channels;
    g_audio.mic_vol     = mic_vol;
    g_audio.sys_vol     = sys_vol;
    g_audio.running.store(true);

    if (!g_audio.mic_path.empty())
        g_audio.mic_thr = std::thread(MicThread,
            g_audio.mic_path, sample_rate, channels, mic_vol);
    if (!g_audio.sys_path.empty())
        g_audio.sys_thr = std::thread(SysAudioThread,
            g_audio.sys_path, sample_rate, channels, sys_vol);
    return 0;
}
extern "C" void HR_AudioStop() {
    g_audio.running.store(false);
    if (g_audio.mic_thr.joinable()) g_audio.mic_thr.join();
    if (g_audio.sys_thr.joinable()) g_audio.sys_thr.join();
}
extern "C" float HR_AudioGetMicLevel() { return g_audio.mic_level.load(); }
extern "C" float HR_AudioGetSysLevel() { return g_audio.sys_level.load(); }

// -------------------- DLL entry point -------------------------------------
BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        HR_SetTimerResolution(1);
    }
    if (reason == DLL_PROCESS_DETACH) {
        HR_PreviewStop();
        HR_RecordStop();
        HR_AudioStop();
        HR_SetTimerResolution(0);
    }
    return TRUE;
}
