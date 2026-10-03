#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <propkeydef.h>
#include <functiondiscoverykeys_devpkey.h>
#include <endpointvolume.h>

#include <atomic>
#include <thread>
#include <mutex>
#include <vector>
#include <string>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <cassert>
#include <exception>
#include <memory>

#include "hr_log.h"

#ifdef _WIN32
  #define HR_EXPORT extern "C" __declspec(dllexport)
#else
  #define HR_EXPORT extern "C" __attribute__((visibility("default")))
#endif

// ---------------------------------------------------------------------------
// WAV helpers
// ---------------------------------------------------------------------------

#pragma pack(push, 1)
struct WavHeader {
    char     riff[4]       = {'R','I','F','F'};
    uint32_t chunk_size    = 0;
    char     wave[4]       = {'W','A','V','E'};
    char     fmt[4]        = {'f','m','t',' '};
    uint32_t subchunk1     = 16;
    uint16_t audio_fmt     = 1;  // PCM
    uint16_t num_channels  = 2;
    uint32_t sample_rate   = 44100;
    uint32_t byte_rate     = 0;
    uint16_t block_align   = 0;
    uint16_t bits_per_smp  = 16;
    char     data[4]       = {'d','a','t','a'};
    uint32_t data_size     = 0;
};
#pragma pack(pop)

static bool wav_write(const char* path,
                      const std::vector<int16_t>& pcm,
                      uint16_t channels,
                      uint32_t rate = 44100)
{
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    WavHeader h;
    h.num_channels = channels;
    h.sample_rate  = rate;
    h.bits_per_smp = 16;
    h.block_align  = channels * 2;
    h.byte_rate    = rate * channels * 2;
    uint32_t data_bytes = (uint32_t)(pcm.size() * 2);
    h.data_size    = data_bytes;
    h.chunk_size   = 36 + data_bytes;
    fwrite(&h, sizeof(h), 1, f);
    fwrite(pcm.data(), 2, pcm.size(), f);
    fclose(f);
    return true;
}

// ---------------------------------------------------------------------------
// Incremental WAV streaming
//
// A manual recording used to hold its ENTIRE mic/system audio track as raw
// 16-bit PCM in mic_buf/sys_buf (see AudioState::max_buffer_sec's comment -
// 0 during a manual recording means "don't trim, keep everything") until
// Stop() finally called wav_write() once with the whole thing. At 44.1kHz
// stereo that's ~176KB/sec per stream (~350KB/sec for both), so a 10-minute
// recording held over 200MB of raw audio in RAM for its whole duration on
// top of everything else the app was already using - exactly the steady,
// recording-length-proportional RAM growth ("оперативка утекает", 164MB ->
// 210MB and climbing) users were seeing, even though every byte of it *was*
// eventually freed at Stop() (not a leak in the classic sense, just an
// unbounded working set for as long as the recording ran).
//
// Fix: write the header up front with placeholder sizes, append PCM
// straight to disk as it's periodically flushed out of mic_buf/sys_buf
// (see hr_audio_flush_buffered(), called from RecordingController's
// existing stats-poll timer), and patch the header's size fields in place
// when the stream closes. RAM now only ever holds a few seconds of audio
// (whatever's accumulated since the last flush) regardless of how long the
// recording runs.
static FILE* wav_stream_open(const char* path)
{
    if (!path || !path[0]) return nullptr;
    FILE* f = fopen(path, "wb");
    if (!f) return nullptr;
    WavHeader h; // placeholder - real channel/rate/size patched in on close
    if (fwrite(&h, sizeof(h), 1, f) != 1) { fclose(f); return nullptr; }
    return f;
}

static void wav_stream_append(FILE* f, const int16_t* data, size_t n)
{
    if (!f || !data || n == 0) return;
    fwrite(data, 2, n, f);
}

// Seeks back to patch the header now that the real channel count/rate/byte
// count are known, then closes the file. Safe to call with data_bytes == 0
// (an empty stream, e.g. a muted mic that never produced samples) - still
// leaves a valid, playable (silent) WAV rather than a truncated one.
static void wav_stream_close(FILE* f, uint16_t channels, uint32_t rate, uint64_t data_bytes)
{
    if (!f) return;
    WavHeader h;
    h.num_channels = channels;
    h.sample_rate  = rate;
    h.bits_per_smp = 16;
    h.block_align  = (uint16_t)(channels * 2);
    h.byte_rate    = rate * channels * 2;
    h.data_size    = (uint32_t)data_bytes;
    h.chunk_size   = 36 + (uint32_t)data_bytes;
    fseek(f, 0, SEEK_SET);
    fwrite(&h, sizeof(h), 1, f);
    fclose(f);
}

static bool wav_read(const char* path,
                     std::vector<int16_t>& pcm,
                     uint16_t& channels,
                     uint32_t& rate)
{
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    WavHeader h;
    if (fread(&h, sizeof(h), 1, f) != 1) { fclose(f); return false; }
    channels = h.num_channels;
    rate     = h.sample_rate;
    size_t n = h.data_size / 2;
    pcm.resize(n);
    fread(pcm.data(), 2, n, f);
    fclose(f);
    return true;
}
static void resample_linear_stereo(std::vector<int16_t>& buf,
                                    uint32_t from_rate, uint32_t to_rate)
{
    if (from_rate == 0 || to_rate == 0 || from_rate == to_rate || buf.empty())
        return;

    const size_t frames_in = buf.size() / 2; // stereo
    const double ratio = (double)to_rate / (double)from_rate;
    const size_t frames_out = (size_t)((double)frames_in * ratio);
    if (frames_out == 0) { buf.clear(); return; }

    std::vector<int16_t> out(frames_out * 2);
    for (size_t i = 0; i < frames_out; ++i) {
        double src_pos = (double)i / ratio;
        size_t i0 = (size_t)src_pos;
        size_t i1 = std::min(i0 + 1, frames_in - 1);
        double frac = src_pos - (double)i0;

        for (int ch = 0; ch < 2; ++ch) {
            double s0 = buf[i0 * 2 + ch];
            double s1 = buf[i1 * 2 + ch];
            double v = s0 + (s1 - s0) * frac;
            if (v > 32767.0) v = 32767.0;
            if (v < -32768.0) v = -32768.0;
            out[i * 2 + ch] = (int16_t)v;
        }
    }
    buf = std::move(out);
}

// ---------------------------------------------------------------------------
// RMS level 0-100
// ---------------------------------------------------------------------------
static int calc_rms(const int16_t* buf, size_t n)
{
    if (!n) return 0;
    double sum = 0.0;
    for (size_t i = 0; i < n; ++i)
        sum += (double)buf[i] * buf[i];
    double rms = std::sqrt(sum / n);
    if (rms < 1.0) return 0;  // true digital silence

    // A raw *linear* divide such as `min(100, (int)(rms / 150.0))` would
    // require an RMS of ~15000 (already quite loud -- roughly half of full
    // scale, 32768) just to reach 100 on the meter, and anything under ~150
    // would round straight down to 0. Ordinary speech and background music
    // RMS almost always lives in the low hundreds to low thousands
    // (full-scale-amplitude audio is rare outside test tones), so a linear
    // scale would peg the meter at (or a hair above) 0 for completely
    // normal mic/system audio -- it would look like "complete silence" even
    // with clearly audible input. Human hearing -- and every real VU/level
    // meter -- is logarithmic, not linear, so scale dBFS instead: map the
    // [-50 dBFS, 0 dBFS] range (where normal mic/speaker levels actually
    // live) onto [0, 100].
    double dbfs = 20.0 * std::log10(rms / 32768.0);
    const double floor_db = -50.0;
    double pct = (dbfs - floor_db) / (0.0 - floor_db) * 100.0;
    if (pct < 0.0)   pct = 0.0;
    if (pct > 100.0) pct = 100.0;
    return (int)(pct + 0.5);
}

// ---------------------------------------------------------------------------
// WASAPI stream wrapper
// ---------------------------------------------------------------------------
struct WasapiStream {
    IMMDeviceEnumerator*  enumerator  = nullptr;
    IMMDevice*            device      = nullptr;
    IAudioClient*         client      = nullptr;
    IAudioCaptureClient*  capture     = nullptr;
    WAVEFORMATEX*         mix_fmt     = nullptr;
    bool                  loopback    = false;
    uint16_t              channels    = 2;
    uint32_t              rate        = 44100;
    HANDLE                data_event  = nullptr;

    // Wall-clock bookkeeping for loopback gap filling (see read()).
    bool                  clock_started = false;
    LARGE_INTEGER         clock_t0{};
    LARGE_INTEGER         clock_freq{};
    int64_t               frames_total = 0;   // stereo frames delivered (incl. padding) since clock_t0

    // 2.4: per-process loopback (a browser / one window's sound) - defined in hr_audio_extra.inc
    bool open_process(unsigned long pid);

    bool open(bool is_loopback, IMMDevice* dev)
    {
        loopback = is_loopback;
        device   = dev;
        device->AddRef();

        // (handle/COM leak on any Start() that fails partway):
        // every early-return below used to just `return false` once
        // device/client/mix_fmt/data_event/capture had already been
        // partially acquired - leaking whichever of those had already been
        // set (device's extra AddRef() above included) instead of releasing
        // them. On its own this is a slow leak (repeatedly failing to open
        // a WASAPI stream - a device that's unplugged, in exclusive use by
        // another app, or a bad mic_device_id from Settings - just leaked a
        // little more each time), but since Settings can call hr_audio_start()
        // to preview a device, that "occasionally fails" path could be
        // exercised often enough to add up over an app session. Every
        // failure path now calls close() (defined below - already null-
        // checks each field) before returning, exactly like the success
        // path's own eventual teardown.
        HRESULT hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL,
                                      nullptr, (void**)&client);
        if (FAILED(hr)) { close(); return false; }

        hr = client->GetMixFormat(&mix_fmt);
        if (FAILED(hr)) { close(); return false; }

        channels = (uint16_t)mix_fmt->nChannels;
        rate     = mix_fmt->nSamplesPerSec;

        AUDCLNT_SHAREMODE mode = AUDCLNT_SHAREMODE_SHARED;
        DWORD flags = (is_loopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0)
                      | AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
        hr = client->Initialize(mode, flags,
                                2000000LL, 0, mix_fmt, nullptr);
        if (FAILED(hr)) { close(); return false; }

        data_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!data_event) { close(); return false; }
        hr = client->SetEventHandle(data_event);
        if (FAILED(hr)) { close(); return false; }

        hr = client->GetService(__uuidof(IAudioCaptureClient),
                                (void**)&capture);
        if (FAILED(hr)) { close(); return false; }

        hr = client->Start();
        if (FAILED(hr)) { close(); return false; }
        QueryPerformanceFrequency(&clock_freq);
        QueryPerformanceCounter(&clock_t0);
        frames_total  = 0;
        clock_started = true;
        return true;
    }

    // Used while recording is paused: throw away whatever WASAPI queued up
    // (up to its 2s buffer) so it isn't appended after resume, and restart
    // the wall-clock baseline so the paused time isn't padded as silence.
    void drain_and_rebase()
    {
        if (capture) {
            UINT32 pkt = 0;
            while (SUCCEEDED(capture->GetNextPacketSize(&pkt)) && pkt > 0) {
                BYTE* d = nullptr; UINT32 n = 0; DWORD f = 0;
                if (FAILED(capture->GetBuffer(&d, &n, &f, nullptr, nullptr))) break;
                capture->ReleaseBuffer(n);
            }
        }
        clock_started = false;
    }

    // Read available frames, convert to int16 stereo 44100
    // Returns number of int16 samples written to out (interleaved)
    int read(std::vector<int16_t>& out)
    {
        if (!capture) return 0;
        int total = 0;
        UINT32 pkt = 0;
        const size_t start_idx = out.size();
        while (SUCCEEDED(capture->GetNextPacketSize(&pkt)) && pkt > 0) {
            BYTE* data = nullptr;
            UINT32 n   = 0;
            DWORD  flags = 0;
            if (FAILED(capture->GetBuffer(&data, &n, &flags, nullptr, nullptr)))
                break;

            bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;

            // mix_fmt is always float (WAVE_FORMAT_IEEE_FLOAT) in WASAPI shared
            // Convert float → int16, down/up-mix channels, resample if needed
            bool is_float = (mix_fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                             (mix_fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                              mix_fmt->cbSize >= 22));

            size_t prev = out.size();
            out.resize(prev + n * 2); // 2 = target stereo
            int16_t* dst = out.data() + prev;

            if (silent) {
                memset(dst, 0, n * 2 * sizeof(int16_t));
            } else if (is_float) {
                const float* src = (const float*)data;
                uint32_t src_ch  = mix_fmt->nChannels;
                for (UINT32 i = 0; i < n; ++i) {
                    float l = src[i * src_ch];
                    float r = (src_ch > 1) ? src[i * src_ch + 1] : l;
                    dst[i*2]   = (int16_t)(std::max(-1.0f, std::min(1.0f, l)) * 32767.f);
                    dst[i*2+1] = (int16_t)(std::max(-1.0f, std::min(1.0f, r)) * 32767.f);
                }
            } else {
                // int16 input
                const int16_t* src = (const int16_t*)data;
                uint32_t src_ch    = mix_fmt->nChannels;
                for (UINT32 i = 0; i < n; ++i) {
                    dst[i*2]   = src[i * src_ch];
                    dst[i*2+1] = (src_ch > 1) ? src[i * src_ch + 1] : src[i * src_ch];
                }
            }
            total += (int)(n * 2);
            capture->ReleaseBuffer(n);
        }

        // A/V SYNC FIX (system audio "runs ahead" of the video): WASAPI
        // loopback delivers NO packets at all while nothing is playing - the
        // silence is simply absent, not sent as zeros. Every silent stretch
        // therefore made the system-audio track shorter than real time, and
        // everything after it played EARLIER than the video it belongs to
        // (the drift grows with the total silence). The mic stream doesn't
        // have this problem (it delivers continuously), so only loopback is
        // padded: compare the frames delivered so far with the wall-clock
        // time elapsed and insert the missing silence BEFORE this call's data
        // (the gap precedes it). Small deficits (< 50ms: normal packet
        // jitter / device-clock ppm drift) are left alone.
        if (loopback && rate > 0) {
            LARGE_INTEGER now{};
            QueryPerformanceCounter(&now);
            if (!clock_started) {
                clock_t0 = now;
                frames_total = 0;
                clock_started = true;
            }
            const int64_t frames_new = (int64_t)(out.size() - start_idx) / 2;
            const double elapsed_frames = clock_freq.QuadPart > 0
                ? (double)(now.QuadPart - clock_t0.QuadPart) * (double)rate / (double)clock_freq.QuadPart
                : 0.0;
            const int64_t deficit = (int64_t)elapsed_frames - (frames_total + frames_new);
            if (deficit > (int64_t)rate / 20) {
                out.insert(out.begin() + (ptrdiff_t)start_idx, (size_t)deficit * 2, (int16_t)0);
                frames_total += deficit;
                total += (int)(deficit * 2);
            }
            frames_total += frames_new;
        }
        return total;
    }

    void close()
    {
        if (client)   { client->Stop(); }
        if (capture)  { capture->Release();  capture  = nullptr; }
        if (client)   { client->Release();   client   = nullptr; }
        if (device)   { device->Release();   device   = nullptr; }
        if (mix_fmt)  { CoTaskMemFree(mix_fmt); mix_fmt = nullptr; }
        if (enumerator) { enumerator->Release(); enumerator = nullptr; }
        if (data_event) { CloseHandle(data_event); data_event = nullptr; }
    }
};

// ---------------------------------------------------------------------------
// Device helpers
// ---------------------------------------------------------------------------
static IMMDeviceEnumerator* make_enumerator()
{
    IMMDeviceEnumerator* e = nullptr;
    CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                     CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&e);
    return e;
}

static IMMDevice* get_default_output(IMMDeviceEnumerator* e)
{
    IMMDevice* dev = nullptr;
    e->GetDefaultAudioEndpoint(eRender, eConsole, &dev);
    return dev;
}

// Find the input device whose endpoint ID matches exactly -- unlike
// find_input_by_name() above (fuzzy, used only for the Stereo Mix
// fallback), this backs an explicit user choice from Settings (see
// hr_mic_enum.h's HrEnumerateMics()/AppState::mic_device_id), so it needs
// to find *that* device or none at all, never something else that merely
// sounds similar.
static IMMDevice* find_input_by_id(IMMDeviceEnumerator* e, const wchar_t* id)
{
    if (!id || !*id) return nullptr;
    IMMDevice* dev = nullptr;
    e->GetDevice(id, &dev);
    return dev;
}

static IMMDevice* get_default_input(IMMDeviceEnumerator* e)
{
    IMMDevice* dev = nullptr;
    e->GetDefaultAudioEndpoint(eCapture, eConsole, &dev);
    return dev;
}

// Find first input device whose name contains any of the keywords
static IMMDevice* find_input_by_name(IMMDeviceEnumerator* e,
                                     const wchar_t* const* kws, int nkw)
{
    IMMDeviceCollection* col = nullptr;
    if (FAILED(e->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &col)))
        return nullptr;
    UINT cnt = 0; col->GetCount(&cnt);
    for (UINT i = 0; i < cnt; ++i) {
        IMMDevice* dev = nullptr;
        if (FAILED(col->Item(i, &dev))) continue;
        IPropertyStore* ps = nullptr;
        if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &ps))) {
            PROPVARIANT pv; PropVariantInit(&pv);
            if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &pv)) &&
                pv.vt == VT_LPWSTR)
            {
                std::wstring nm(pv.pwszVal);
                std::wstring nl = nm;
                for (auto& c : nl) c = towlower(c);
                for (int k = 0; k < nkw; ++k) {
                    if (nl.find(kws[k]) != std::wstring::npos) {
                        PropVariantClear(&pv);
                        ps->Release();
                        col->Release();
                        return dev;
                    }
                }
            }
            PropVariantClear(&pv);
            ps->Release();
        }
        dev->Release();
    }
    col->Release();
    return nullptr;
}

// ---------------------------------------------------------------------------
// Recording state
// ---------------------------------------------------------------------------
struct AudioState {
    // Mic
    WasapiStream        mic_stream;
    std::thread         mic_thread;
    std::vector<int16_t> mic_buf;
    std::mutex          mic_mutex;
    std::atomic<int>    mic_level{0};

    // Sys
    WasapiStream        sys_stream;
    std::thread         sys_thread;
    std::vector<int16_t> sys_buf;
    std::mutex          sys_mutex;
    std::atomic<int>    sys_level{0};

    // Control
    std::atomic<bool>   running{false};
    std::atomic<bool>   paused{false};

    // mic_buf/sys_buf below used to be
    // appended to unconditionally, every ~10ms, for as long as the app
    // was open - including all the time spent NOT recording, since the
    // capture threads run continuously from startup so the live level
    // meters keep working. Nothing ever drained that idle-time audio
    // (hr_audio_reset_buffers()/hr_audio_capture_to_wav() only run at
    // actual Start()/Stop()), so it just accumulated raw 44.1kHz stereo
    // PCM from both streams for as long as the app sat idle - tens of MB
    // after a few minutes AFK, unbounded the longer it was left running.
    // This flag gates the buffer *writes* only; level metering below
    // reads straight from each read()'s fresh chunk regardless of it, so
    // the meters keep working while idle exactly as before. Set true by
    // hr_audio_reset_buffers() (Start()), false by
    // hr_audio_capture_to_wav()/hr_audio_stop() (Stop()/app exit).
    std::atomic<bool>   buffering{false};
    std::atomic<int>    max_buffer_sec{0};
    std::mutex stream_mutex; // serializes hr_audio_flush_buffered() (UI thread) vs stream close (finalize thread)
    FILE*    mic_stream_file  = nullptr;
    FILE*    sys_stream_file  = nullptr;
    uint64_t mic_stream_bytes = 0;
    uint64_t sys_stream_bytes = 0;

    // Volume/mute (written from the UI thread, read from the audio threads)
    std::atomic<float>  mic_vol{1.0f};
    std::atomic<float>  sys_vol{1.0f};
    std::atomic<bool>   mic_mute{false};
    std::atomic<bool>   sys_mute{false};
    // Gain actually applied to the last sample of the previous chunk -
    // each worker's own running state, touched only by that one thread
    // (unlike mic_vol/sys_vol above, which the UI thread writes and this
    // thread reads) - see mic_worker()/sys_worker()'s ramp comment for
    // why this exists.
    float mic_vol_smooth = 1.0f;
    float sys_vol_smooth = 1.0f;
};

static AudioState* g_state = nullptr;

// ---------------------------------------------------------------------------
// Capture threads
// ---------------------------------------------------------------------------
static void mic_worker(AudioState* st)
{
    try {
    const int SLEEP_MS = 10;
    while (st->running.load()) {
        if (st->paused.load()) {
            st->mic_stream.drain_and_rebase();
            Sleep(SLEEP_MS);
            continue;
        }
        if (st->mic_stream.data_event)
            WaitForSingleObject(st->mic_stream.data_event, SLEEP_MS);
        else
            Sleep(SLEEP_MS);  // fallback poll if event setup failed
        std::vector<int16_t> tmp;
        st->mic_stream.read(tmp);

        if (!tmp.empty()) {
            float target_vol = st->mic_vol.load();
            bool  mute = st->mic_mute.load();
            if (mute) {
                memset(tmp.data(), 0, tmp.size() * 2);
                st->mic_vol_smooth = target_vol; // don't ramp *into* a stale value on unmute
            } else if (target_vol != st->mic_vol_smooth) {
                // Ramp linearly from the gain applied to the previous
                // chunk's last sample up to wherever the slider is *now*,
                // one step per sample, instead of one flat multiply for
                // the whole chunk. A slider drag lands its new value
                // between one ~10ms audio chunk and the next (SLEEP_MS
                // above) - applying it as a single scalar for the entire
                // next chunk is an audible step, heard as choppy/
                // zippering volume changes rather than a smooth fade.
                // Same per-sample multiply-and-clamp cost as the flat
                // version below, just an incrementing gain instead of a
                // constant one - no extra passes, no extra CPU to speak of.
                float start_vol = st->mic_vol_smooth;
                size_t n = tmp.size();
                for (size_t i = 0; i < n; ++i) {
                    float g = start_vol + (target_vol - start_vol) * ((float)(i + 1) / (float)n);
                    int v = (int)(tmp[i] * g);
                    tmp[i] = (int16_t)std::max(-32768, std::min(32767, v));
                }
                st->mic_vol_smooth = target_vol;
            } else if (target_vol != 1.0f) {
                for (auto& s : tmp) {
                    int v = (int)(s * target_vol);
                    s = (int16_t)std::max(-32768, std::min(32767, v));
                }
            }
            st->mic_level.store(mute ? 0 : calc_rms(tmp.data(), tmp.size()));
            if (st->buffering.load()) {
                std::lock_guard<std::mutex> lk(st->mic_mutex);
                st->mic_buf.insert(st->mic_buf.end(), tmp.begin(), tmp.end());
                int cap_sec = st->max_buffer_sec.load();
                if (cap_sec > 0 && st->mic_stream.rate > 0) {
                    size_t max_samples = (size_t)cap_sec * st->mic_stream.rate * 2; // stereo int16
                    // PERF: trim in ~1s chunks, not on every 10ms read. Once the
                    // rolling buffer is full, erasing from the front of a
                    // std::vector memmoves the whole buffer (megabytes) - doing
                    // that 100x/sec while holding the mutex was pure overhead.
                    // The buffer may now exceed the cap by up to 1s; every
                    // reader trims to the exact cap (see ring_window_start()).
                    if (st->mic_buf.size() > max_samples + (size_t)st->mic_stream.rate * 2)
                        st->mic_buf.erase(st->mic_buf.begin(), st->mic_buf.end() - (ptrdiff_t)max_samples);
                }
            }
        }
        // No trailing Sleep here anymore -- the wait at the top of the loop
        // (event or fallback poll) already paces this thread; sleeping again
        // here would just add a second, redundant delay on top of it.
    }
    } catch (const std::exception &e) {
        HrLog::Error(std::string("Mic capture thread: uncaught exception (") + e.what() + ")");
    } catch (...) {
        HrLog::Error("Mic capture thread: uncaught unknown exception");
    }
}

static void sys_worker(AudioState* st)
{
    try {
    const int SLEEP_MS = 10;
    while (st->running.load()) {
        if (st->paused.load()) {
            st->sys_stream.drain_and_rebase();
            Sleep(SLEEP_MS);
            continue;
        }
        if (st->sys_stream.data_event)
            WaitForSingleObject(st->sys_stream.data_event, SLEEP_MS);
        else
            Sleep(SLEEP_MS);  // fallback poll if event setup failed
        std::vector<int16_t> tmp;
        st->sys_stream.read(tmp);

        if (!tmp.empty()) {
            float target_vol = st->sys_vol.load();
            bool  mute = st->sys_mute.load();
            if (mute) {
                memset(tmp.data(), 0, tmp.size() * 2);
                st->sys_vol_smooth = target_vol; // don't ramp *into* a stale value on unmute
            } else if (target_vol != st->sys_vol_smooth) {
                // See the matching comment in mic_worker() above.
                float start_vol = st->sys_vol_smooth;
                size_t n = tmp.size();
                for (size_t i = 0; i < n; ++i) {
                    float g = start_vol + (target_vol - start_vol) * ((float)(i + 1) / (float)n);
                    int v = (int)(tmp[i] * g);
                    tmp[i] = (int16_t)std::max(-32768, std::min(32767, v));
                }
                st->sys_vol_smooth = target_vol;
            } else if (target_vol != 1.0f) {
                for (auto& s : tmp) {
                    int v = (int)(s * target_vol);
                    s = (int16_t)std::max(-32768, std::min(32767, v));
                }
            }
            st->sys_level.store(mute ? 0 : calc_rms(tmp.data(), tmp.size()));
            if (st->buffering.load()) {
                std::lock_guard<std::mutex> lk(st->sys_mutex);
                st->sys_buf.insert(st->sys_buf.end(), tmp.begin(), tmp.end());
                int cap_sec = st->max_buffer_sec.load();
                if (cap_sec > 0 && st->sys_stream.rate > 0) {
                    size_t max_samples = (size_t)cap_sec * st->sys_stream.rate * 2; // stereo int16
                    // PERF: trim in ~1s chunks, not on every 10ms read. Once the
                    // rolling buffer is full, erasing from the front of a
                    // std::vector memmoves the whole buffer (megabytes) - doing
                    // that 100x/sec while holding the mutex was pure overhead.
                    // The buffer may now exceed the cap by up to 1s; every
                    // reader trims to the exact cap (see ring_window_start()).
                    if (st->sys_buf.size() > max_samples + (size_t)st->sys_stream.rate * 2)
                        st->sys_buf.erase(st->sys_buf.begin(), st->sys_buf.end() - (ptrdiff_t)max_samples);
                }
            }
        }
        // See matching comment in mic_worker() above.
    }
    } catch (const std::exception &e) {
        HrLog::Error(std::string("System-audio capture thread: uncaught exception (") + e.what() + ")");
    } catch (...) {
        HrLog::Error("System-audio capture thread: uncaught unknown exception");
    }
}

// ---------------------------------------------------------------------------
// Public C API
// ---------------------------------------------------------------------------

/*  hr_audio_init()
    Вызвать один раз при старте.
    Возвращает 0 при успехе, отрицательное - ошибка CoInitialize. */
HR_EXPORT int hr_audio_init()
{
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) return -1;
    return 0;
}

/*  hr_audio_start(mic_vol, sys_vol, mic_mute, sys_mute, mic_device_id)
    Открывает потоки и запускает потоки записи.
    mic_device_id: WASAPI endpoint ID from HrEnumerateMics() (hr_mic_enum.h),
    or nullptr/empty to keep using whatever Windows currently considers the
    default recording device (previous, and still the default, behavior).
    Возвращает битовую маску: bit0=mic_ok, bit1=sys_ok. */
HR_EXPORT int hr_audio_start(float mic_vol, float sys_vol,
                              int mic_mute, int sys_mute,
                              const wchar_t* mic_device_id)
{
    if (g_state && g_state->running.load())
        return -1;  // уже запущено

    delete g_state;
    g_state = new AudioState();
    g_state->mic_vol.store(mic_vol);
    g_state->sys_vol.store(sys_vol);
    g_state->mic_mute.store(mic_mute != 0);
    g_state->sys_mute.store(sys_mute != 0);

    IMMDeviceEnumerator* enumerator = make_enumerator();
    if (!enumerator) { delete g_state; g_state = nullptr; return 0; }

    int result = 0;

    // ---- Mic ---------------------------------------------------------------
    // This always opened whatever Windows considered the default
    // capture device, with no way to record from a different one -- see
    // Settings' new microphone picker (settings_dialog.cpp) which is the
    // first thing to actually set mic_device_id to something. Falls back to
    // the default device if the requested one isn't specified, or can't be
    // found anymore (unplugged since it was chosen, etc).
    IMMDevice* mic_dev = find_input_by_id(enumerator, mic_device_id);
    if (!mic_dev) mic_dev = get_default_input(enumerator);
    if (mic_dev && g_state->mic_stream.open(false, mic_dev)) {
        result |= 0x1;
        mic_dev->Release();
        g_state->running.store(true);
        g_state->mic_thread = std::thread(mic_worker, g_state);
    } else if (mic_dev) {
        mic_dev->Release();
    }

    // ---- Sys (loopback) ----------------------------------------------------
    // Priority: default output loopback → Stereo Mix input device
    IMMDevice* sys_dev = get_default_output(enumerator);
    bool sys_ok = false;
    if (sys_dev) {
        if (g_state->sys_stream.open(true, sys_dev)) {
            sys_ok = true;
        }
        sys_dev->Release();
    }
    if (!sys_ok) {
        // Try Stereo Mix / "что слышит" input device
        const wchar_t* kws[] = {
            L"stereo mix", L"what u hear", L"loopback",
            L"\u0441\u0442\u0435\u0440\u0435\u043e",   // "стерео"
            L"\u043c\u0438\u043a\u0448\u0435\u0440",   // "микшер"
        };
        IMMDevice* sm_dev = find_input_by_name(enumerator, kws, 5);
        if (sm_dev) {
            if (g_state->sys_stream.open(false, sm_dev))
                sys_ok = true;
            sm_dev->Release();
        }
    }
    if (sys_ok) {
        result |= 0x2;
        if (!g_state->running.load()) g_state->running.store(true);
        g_state->sys_thread = std::thread(sys_worker, g_state);
    }

    enumerator->Release();
    return result;
}

HR_EXPORT void hr_audio_set_volumes(float mic_vol, float sys_vol,
                                     int mic_mute, int sys_mute)
{
    if (!g_state) return;
    g_state->mic_vol.store(mic_vol);
    g_state->sys_vol.store(sys_vol);
    g_state->mic_mute.store(mic_mute != 0);
    g_state->sys_mute.store(sys_mute != 0);
}

HR_EXPORT void hr_audio_get_levels(int* out_mic, int* out_sys)
{
    if (!g_state) { if(out_mic) *out_mic=0; if(out_sys) *out_sys=0; return; }
    if (out_mic) *out_mic = g_state->mic_level.load();
    if (out_sys) *out_sys = g_state->sys_level.load();
}

static void extra_on_pause(bool paused);   // hr_audio_extra.inc

HR_EXPORT void hr_audio_pause(int paused)
{
    if (g_state) g_state->paused.store(paused != 0);
    extra_on_pause(paused != 0);
}

HR_EXPORT int hr_audio_stop(const char* mic_wav_path,
                             const char* sys_wav_path)
{
    if (!g_state) return 0;
    g_state->running.store(false);

    // Join the capture threads BEFORE releasing the WASAPI objects they're
    // using. The previous order called close() (which Release()s client/
    // capture) first, while mic_worker/sys_worker could still be mid-call
    // inside WasapiStream::read() (GetNextPacketSize/GetBuffer/ReleaseBuffer
    // on those exact pointers) - a released-COM-object race that could hang
    // the calling thread instead of failing cleanly, which is what made
    // clicking Stop freeze the whole app. Threads check running.load() at
    // the top of a ~10ms loop, so this join returns quickly once they exit
    // on their own; only *then* is it safe to close() the streams.
    if (g_state->mic_thread.joinable()) g_state->mic_thread.join();
    if (g_state->sys_thread.joinable()) g_state->sys_thread.join();

    g_state->mic_stream.close();
    g_state->sys_stream.close();

    int result = 0;

    if (mic_wav_path) {
        std::lock_guard<std::mutex> lk(g_state->mic_mutex);
        if (!g_state->mic_buf.empty() &&
            wav_write(mic_wav_path, g_state->mic_buf, 2, g_state->mic_stream.rate))
            result |= 0x1;
    }
    if (sys_wav_path) {
        std::lock_guard<std::mutex> lk(g_state->sys_mutex);
        if (!g_state->sys_buf.empty() &&
            wav_write(sys_wav_path, g_state->sys_buf, 2, g_state->sys_stream.rate))
            result |= 0x2;
    }

    delete g_state;
    g_state = nullptr;
    return result;
}

/*  hr_audio_reset_buffers()
    Clears whatever's accumulated in mic_buf/sys_buf so far without
    touching the running capture threads. Call this at the moment an
    actual recording starts, so the WAV eventually written by
    hr_audio_capture_to_wav() only contains audio from that point
    forward - audio capture itself runs continuously from app startup
    (for the live level meters), so without this the file would include
    whatever was captured while the app just sat idle before Start was
    clicked. */
// mic_wav_path/sys_wav_path (new): when non-null, this is a manual
// recording - open incremental WAV streams at these paths right away (see
// wav_stream_open()'s comment) so hr_audio_flush_buffered() has somewhere
// to send PCM as it's periodically drained from RAM instead of it
// accumulating for the whole recording. Pass nullptr for either/both to
// keep the old fully-in-RAM behavior (Instant Replay's rolling window,
// which needs random access to "the last N seconds" and can't be streamed
// straight to disk).
HR_EXPORT void hr_audio_reset_buffers(const char* mic_wav_path, const char* sys_wav_path)
{
    if (!g_state) return;
    {
        std::lock_guard<std::mutex> lk(g_state->mic_mutex);
        g_state->mic_buf.clear();
    }
    {
        std::lock_guard<std::mutex> lk(g_state->sys_mutex);
        g_state->sys_buf.clear();
    }
    // Belt-and-suspenders: close out any stream left open from a previous
    // recording (should already be closed by hr_audio_capture_to_wav()) so
    // we never leak a FILE* or write two recordings' audio into one file.
    std::lock_guard<std::mutex> slk(g_state->stream_mutex);
    if (g_state->mic_stream_file) { fclose(g_state->mic_stream_file); g_state->mic_stream_file = nullptr; }
    if (g_state->sys_stream_file) { fclose(g_state->sys_stream_file); g_state->sys_stream_file = nullptr; }
    g_state->mic_stream_bytes = 0;
    g_state->sys_stream_bytes = 0;
    g_state->mic_stream_file = wav_stream_open(mic_wav_path);
    g_state->sys_stream_file = wav_stream_open(sys_wav_path);
    // Start buffering PCM again now that an actual recording is underway
    // - see AudioState::buffering's comment for why this was off.
    g_state->buffering.store(true);
}

/*  hr_audio_flush_buffered()
    Drains whatever's currently sitting in mic_buf/sys_buf out to their
    open incremental WAV streams (see wav_stream_open()'s comment) and
    clears the in-RAM copy, so a long manual recording's memory use stays
    flat instead of growing for its entire duration. No-op for either
    stream that doesn't have a stream file open (Instant Replay's ring
    buffer, or a manual recording whose hr_audio_reset_buffers() call
    wasn't given a path for that channel) - that stream keeps behaving
    exactly as before, fully in RAM.

    Meant to be called periodically (a few times a second is plenty - see
    RecordingController::PollStats(), which already runs on a timer for
    the whole duration of a recording) while state_.recording is true; also
    called once more from hr_audio_capture_to_wav() to catch whatever
    accumulated since the last periodic call before the stream closes. */
static void extra_flush();                 // hr_audio_extra.inc

HR_EXPORT void hr_audio_flush_buffered()
{
    extra_flush();   // 2.4: extra mixer channels stream to disk on the same tick
    if (!g_state) return;
    std::lock_guard<std::mutex> slk(g_state->stream_mutex);
    if (g_state->mic_stream_file) {
        std::vector<int16_t> chunk;
        {
            std::lock_guard<std::mutex> lk(g_state->mic_mutex);
            chunk.swap(g_state->mic_buf);
        }
        if (!chunk.empty()) {
            wav_stream_append(g_state->mic_stream_file, chunk.data(), chunk.size());
            g_state->mic_stream_bytes += chunk.size() * 2;
        }
    }
    if (g_state->sys_stream_file) {
        std::vector<int16_t> chunk;
        {
            std::lock_guard<std::mutex> lk(g_state->sys_mutex);
            chunk.swap(g_state->sys_buf);
        }
        if (!chunk.empty()) {
            wav_stream_append(g_state->sys_stream_file, chunk.data(), chunk.size());
            g_state->sys_stream_bytes += chunk.size() * 2;
        }
    }
}

/*  hr_audio_capture_to_wav(mic_wav_path, sys_wav_path)
    Same WAV-writing behavior as hr_audio_stop(), but does NOT stop the
    capture threads or tear down the WASAPI streams - used when a
    *recording* stops but the app itself stays open, so the mic/system
    level meters keep working afterward instead of going dead until the
    next recording starts. Buffers are cleared after writing (mirrors
    hr_audio_reset_buffers()'s job at the other end) so a subsequent
    recording doesn't pick up leftover audio from the gap in between.
    Real teardown (thread stop + WASAPI release) only happens via
    hr_audio_stop(), which should be called once at actual app exit. */
HR_EXPORT int hr_audio_capture_to_wav(const char* mic_wav_path,
                                        const char* sys_wav_path)
{
    if (!g_state) return 0;
    int result = 0;

    // Stop buffering PCM until the next recording starts - see
    // AudioState::buffering's comment. Set before taking the buffers below so
    // there's no window where a worker thread could sneak in one more insert().
    g_state->buffering.store(false);

    // Catch whatever accumulated since the last periodic
    // hr_audio_flush_buffered() call (see its own comment) before closing
    // the streams out below - otherwise the tail end of the recording
    // (up to one flush interval's worth) would be silently dropped.
    hr_audio_flush_buffered();

    std::lock_guard<std::mutex> slk(g_state->stream_mutex);
    if (g_state->mic_stream_file) {
        // Streaming was open for this recording (hr_audio_reset_buffers()
        // was given a real mic path) - finalize that file's header now
        // that the real byte count is known, regardless of mic_wav_path
        // here: the caller may pass nullptr for a muted mic even though a
        // (silent - mute zeroes samples rather than skipping them, see
        // mic_worker()) stream was still opened and written the whole
        // time, and it needs a valid, non-corrupt header either way.
        wav_stream_close(g_state->mic_stream_file, 2, g_state->mic_stream.rate, g_state->mic_stream_bytes);
        g_state->mic_stream_file = nullptr;
        if (mic_wav_path && g_state->mic_stream_bytes > 0) result |= 0x1;
        g_state->mic_stream_bytes = 0;
    } else if (mic_wav_path) {
        // Fallback for a caller that never opened streaming (e.g. an older
        // hr_audio_reset_buffers(nullptr, ...) call site, or Instant
        // Replay's "Save Replay" reusing this function) - same one-shot
        // write hr_audio_capture_to_wav() always used to do.
        std::vector<int16_t> mic_out;
        {
            std::lock_guard<std::mutex> lk(g_state->mic_mutex);
            mic_out.swap(g_state->mic_buf);
        }
        if (!mic_out.empty() && wav_write(mic_wav_path, mic_out, 2, g_state->mic_stream.rate))
            result |= 0x1;
    }

    if (g_state->sys_stream_file) {
        wav_stream_close(g_state->sys_stream_file, 2, g_state->sys_stream.rate, g_state->sys_stream_bytes);
        g_state->sys_stream_file = nullptr;
        if (sys_wav_path && g_state->sys_stream_bytes > 0) result |= 0x2;
        g_state->sys_stream_bytes = 0;
    } else if (sys_wav_path) {
        std::vector<int16_t> sys_out;
        {
            std::lock_guard<std::mutex> lk(g_state->sys_mutex);
            sys_out.swap(g_state->sys_buf);
        }
        if (!sys_out.empty() && wav_write(sys_wav_path, sys_out, 2, g_state->sys_stream.rate))
            result |= 0x2;
    }
    return result;
}

/*  hr_audio_set_max_buffer_sec(seconds)
    See AudioState::max_buffer_sec's own comment. Pass 0 before/at every
    manual-recording Start() (unlimited - the whole recording must be kept),
    and replay_buffer_sec (plus a little slack) whenever Instant Replay is
    buffering with no manual recording running, so its background audio
    capture doesn't grow unbounded for as long as the app is left open. */
HR_EXPORT void hr_audio_set_max_buffer_sec(int seconds)
{
    if (!g_state) return;
    g_state->max_buffer_sec.store(seconds > 0 ? seconds : 0);
}

/*  hr_audio_snapshot_to_wav(mic_wav_path, sys_wav_path)
    Same WAV-writing behavior as hr_audio_capture_to_wav(), but does NOT
    clear the buffers or touch `buffering` afterward - used by Instant
    Replay's "Save Replay" so the rolling audio window keeps accumulating
    for the *next* save instead of being reset to empty (a manual
    recording's Stop() wants the destructive version; a background replay
    buffer being sampled does not - see hr_audio_capture_to_wav()'s own
    comment for that case). */
// First sample index of the window a rolling buffer should expose: the last
// max_buffer_sec seconds (the worker threads only trim in ~1s chunks now).
// Always even, so stereo frames stay aligned. Caller holds the stream's mutex.
static size_t ring_window_start(size_t size, int cap_sec, int rate)
{
    if (cap_sec <= 0 || rate <= 0) return 0;
    const size_t max_samples = (size_t)cap_sec * (size_t)rate * 2;
    return size > max_samples ? size - max_samples : 0;
}

HR_EXPORT int hr_audio_snapshot_to_wav(const char* mic_wav_path,
                                         const char* sys_wav_path)
{
    if (!g_state) return 0;
    int result = 0;
    // PERF: copy the window under the lock, write to disk after releasing it,
    // so saving a replay no longer stalls the capture threads (audible as a
    // glitch in the still-running buffer) for the duration of the file write.
    const int cap_sec = g_state->max_buffer_sec.load();
    std::vector<int16_t> mic_copy, sys_copy;
    int mic_rate = 0, sys_rate = 0;
    if (mic_wav_path) {
        std::lock_guard<std::mutex> lk(g_state->mic_mutex);
        mic_rate = g_state->mic_stream.rate;
        const size_t st = ring_window_start(g_state->mic_buf.size(), cap_sec, mic_rate);
        mic_copy.assign(g_state->mic_buf.begin() + (ptrdiff_t)st, g_state->mic_buf.end());
    }
    if (sys_wav_path) {
        std::lock_guard<std::mutex> lk(g_state->sys_mutex);
        sys_rate = g_state->sys_stream.rate;
        const size_t st = ring_window_start(g_state->sys_buf.size(), cap_sec, sys_rate);
        sys_copy.assign(g_state->sys_buf.begin() + (ptrdiff_t)st, g_state->sys_buf.end());
    }
    if (!mic_copy.empty() && wav_write(mic_wav_path, mic_copy, 2, mic_rate)) result |= 0x1;
    if (!sys_copy.empty() && wav_write(sys_wav_path, sys_copy, 2, sys_rate)) result |= 0x2;
    return result;
}

/*  hr_audio_mix_wav(mic_wav, sys_wav, out_wav)
    Смешивает два WAV файла в один (без normalize, без subprocess).
    Возвращает 0 при успехе. */
HR_EXPORT int hr_audio_mix_wav(const char* mic_path,
                                const char* sys_path,
                                const char* out_path)
{
    std::vector<int16_t> mic_pcm, sys_pcm;
    uint16_t mic_ch = 2, sys_ch = 2;
    uint32_t mic_rate = 44100, sys_rate = 44100;

    if (!wav_read(mic_path, mic_pcm, mic_ch, mic_rate)) return -1;
    if (!wav_read(sys_path, sys_pcm, sys_ch, sys_rate)) return -2;

    // Make both stereo if needed (simple duplication)
    auto to_stereo = [](std::vector<int16_t>& buf, uint16_t ch) {
        if (ch == 2) return;
        std::vector<int16_t> out(buf.size() * 2);
        for (size_t i = 0; i < buf.size(); ++i) {
            out[i*2]   = buf[i];
            out[i*2+1] = buf[i];
        }
        buf = std::move(out);
    };
    to_stereo(mic_pcm, mic_ch);
    to_stereo(sys_pcm, sys_ch);

    // The two WAVs now (post-fix) carry each stream's real native capture
    // rate, which the mic and system-output endpoints are free to differ
    // on. Adding samples 1:1 below assumes both buffers already run at
    // the same rate, so bring the lower-rate one up to the other's rate
    // first -- otherwise the resulting mix plays back correctly for
    // neither track. See resample_linear_stereo()'s comment above.
    uint32_t out_rate = mic_rate;
    if (mic_rate != sys_rate) {
        if (mic_rate < sys_rate) {
            resample_linear_stereo(mic_pcm, mic_rate, sys_rate);
            out_rate = sys_rate;
        } else {
            resample_linear_stereo(sys_pcm, sys_rate, mic_rate);
            out_rate = mic_rate;
        }
    }

    size_t n = std::max(mic_pcm.size(), sys_pcm.size());
    mic_pcm.resize(n, 0);
    sys_pcm.resize(n, 0);

    std::vector<int16_t> out(n);
    for (size_t i = 0; i < n; ++i) {
        int32_t s = (int32_t)mic_pcm[i] + (int32_t)sys_pcm[i];
        // Soft clip
        if      (s >  32767) s =  32767;
        else if (s < -32768) s = -32768;
        out[i] = (int16_t)s;
    }

    return wav_write(out_path, out, 2, out_rate) ? 0 : -3;
}

/*  hr_audio_mix_wav_list(inputs, out_path)   (2.4)
    Mixes any number of 16-bit stereo WAVs (this file's own writers: 44-byte header)
    into one, '|'-separated UTF-8 paths in `inputs`. STREAMING: a few KB of RAM no
    matter how long the recording is - the older hr_audio_mix_wav() above reads both
    inputs whole. Inputs at different sample rates are linearly resampled to the first
    input's rate. Returns 0 on success. */
namespace {
struct MixIn {
    FILE*    f = nullptr;
    uint32_t rate = 44100;
    uint64_t total = 0;          // frames in the file
    int64_t  cur = -1;           // index of frame `a`
    int16_t  a[2] = {0, 0}, b[2] = {0, 0};
    int16_t  blk[4096 * 2];
    size_t   blk_n = 0, blk_i = 0;
    uint64_t consumed = 0;       // frames pulled from the file so far
    bool open(const std::string& path)
    {
        f = fopen(path.c_str(), "rb");
        if (!f) return false;
        WavHeader h;
        if (fread(&h, sizeof(h), 1, f) != 1 || h.num_channels != 2 || h.bits_per_smp != 16) { fclose(f); f = nullptr; return false; }
        rate = h.sample_rate ? h.sample_rate : 44100;
        // The header's size field is patched when a stream closes; if a crash left it 0,
        // fall back to the real file length.
        uint64_t bytes = h.data_size;
        long here = ftell(f);
        fseek(f, 0, SEEK_END);
        long endp = ftell(f);
        fseek(f, here, SEEK_SET);
        if (endp > here && (bytes == 0 || bytes > (uint64_t)(endp - here))) bytes = (uint64_t)(endp - here);
        total = bytes / 4;
        return total > 0;
    }
    bool pull(int16_t o[2])
    {
        if (consumed >= total) return false;
        if (blk_i >= blk_n) {
            blk_n = fread(blk, 4, 4096, f);
            blk_i = 0;
            if (blk_n == 0) return false;
        }
        o[0] = blk[blk_i * 2]; o[1] = blk[blk_i * 2 + 1];
        ++blk_i; ++consumed;
        return true;
    }
    // Sample at a (monotonically increasing) source position. False past the end.
    bool sample(double pos, int16_t out[2])
    {
        int64_t idx = (int64_t)pos;
        if (idx >= (int64_t)total) return false;
        if (cur < 0) {
            if (!pull(a)) return false;
            if (!pull(b)) { b[0] = a[0]; b[1] = a[1]; }
            cur = 0;
        }
        while (cur < idx) {
            a[0] = b[0]; a[1] = b[1];
            if (!pull(b)) { b[0] = a[0]; b[1] = a[1]; }
            ++cur;
        }
        const double fr = pos - (double)idx;
        for (int c = 0; c < 2; ++c) out[c] = (int16_t)(a[c] + (b[c] - a[c]) * fr);
        return true;
    }
    ~MixIn() { if (f) fclose(f); }
};
} // namespace

HR_EXPORT int hr_audio_mix_wav_list(const char* inputs, const char* out_path)
{
    if (!inputs || !out_path) return -1;
    std::vector<std::string> paths;
    {
        std::string cur;
        for (const char* p = inputs; ; ++p) {
            if (*p == '|' || *p == '\0') { if (!cur.empty()) paths.push_back(cur); cur.clear(); if (!*p) break; }
            else cur += *p;
        }
    }
    if (paths.empty()) return -1;

    std::vector<std::unique_ptr<MixIn>> ins;
    for (const auto& p : paths) {
        auto m = std::make_unique<MixIn>();
        if (m->open(p)) ins.push_back(std::move(m));
    }
    if (ins.empty()) return -2;

    const uint32_t out_rate = ins[0]->rate;
    uint64_t out_frames = 0;
    for (auto& m : ins)
        out_frames = std::max<uint64_t>(out_frames, (uint64_t)((double)m->total * out_rate / m->rate));

    FILE* o = wav_stream_open(out_path);
    if (!o) return -3;

    std::vector<int16_t> chunk(8192 * 2);
    std::vector<double> step(ins.size());
    for (size_t k = 0; k < ins.size(); ++k) step[k] = (double)ins[k]->rate / (double)out_rate;

    uint64_t done = 0;
    while (done < out_frames) {
        const size_t n = (size_t)std::min<uint64_t>(8192, out_frames - done);
        for (size_t i = 0; i < n; ++i) {
            int32_t l = 0, r = 0;
            for (size_t k = 0; k < ins.size(); ++k) {
                int16_t s[2];
                if (ins[k]->sample((double)(done + i) * step[k], s)) { l += s[0]; r += s[1]; }
            }
            chunk[i * 2]     = (int16_t)std::max(-32768, std::min(32767, l));
            chunk[i * 2 + 1] = (int16_t)std::max(-32768, std::min(32767, r));
        }
        wav_stream_append(o, chunk.data(), n * 2);
        done += n;
    }
    wav_stream_close(o, 2, out_rate, done * 4);
    return 0;
}

HR_EXPORT int hr_audio_rms(const void* buf, int n_bytes)
{
    return calc_rms((const int16_t*)buf, n_bytes / 2);
}

#include "hr_audio_extra.inc"
