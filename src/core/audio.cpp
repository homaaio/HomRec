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

#include "../utils/log.h"
#include "pcm_ring.h"
#include "wav_util.h"
#include "audio_mixer.h"   // live mixer + resampler (portable, unit-tested)

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

// Incremental WAV streaming
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

// Seeks back to patch the header now that the real channel count/rate/byte count are
// known, then closes the file.
static void wav_stream_close(FILE* f, uint16_t channels, uint32_t rate, uint64_t data_bytes)
{
    if (!f) return;
    WavHeader h;
    h.num_channels = channels;
    h.sample_rate  = rate;
    h.bits_per_smp = 16;
    h.block_align  = (uint16_t)(channels * 2);
    h.byte_rate    = rate * channels * 2;
    // Above 4 GiB (~6.2 h of 48 kHz stereo) the 32-bit size fields can't hold the length: they used to
    // wrap around silently. 0xFFFFFFFF = "until end of file" (ffmpeg reads that fine); HomRec's own
    // readers take the length from the file size (hrwav::EffectiveDataBytes).
    uint32_t riff_sz = 0, data_sz = 0;   // locals: can't bind a reference to a packed field
    hrwav::SizeFields(data_bytes, riff_sz, data_sz);
    h.chunk_size = riff_sz;
    h.data_size  = data_sz;
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
    // Whole-file read: only sane for short files. Refuse anything that wouldn't fit comfortably in RAM
    // instead of allocating gigabytes (or, for a bad header, reading nothing and reporting success).
    uint64_t fsz = 0;
    if (!hrwav::FileSize64(f, fsz) || fsz < sizeof(h)) { fclose(f); return false; }
    const uint64_t bytes = hrwav::EffectiveDataBytes(h.data_size, fsz - sizeof(h));
    if (bytes == 0 || bytes > (1ull << 30)) { fclose(f); return false; }
    size_t n = (size_t)(bytes / 2);
    pcm.resize(n);
    n = fread(pcm.data(), 2, n, f);
    pcm.resize(n);
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

    // A raw *linear* divide such as `min(100, (int)(rms / 150.0))` would require an RMS of
    // ~15000 (already quite loud -- roughly half of full scale, 32768) just to reach 100 on
    // the meter, and anything under ~150 would round straight down to 0.
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

    // Per-recording diagnostics (reset by reset_stats(), reported by log_stats()): how much silence
    // was inserted to keep this stream on the wall clock, how much surplus was dropped, how many
    // times WASAPI itself said "samples were lost" (AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY).
    int64_t               stat_padded_frames  = 0;
    int64_t               stat_trimmed_frames = 0;
    int                   stat_pad_events     = 0;
    int                   stat_trim_events    = 0;
    int                   stat_discontinuities = 0;
    void reset_stats() {
        stat_padded_frames = stat_trimmed_frames = 0;
        stat_pad_events = stat_trim_events = stat_discontinuities = 0;
    }
    void log_stats(const char *who) const {
        if (!stat_pad_events && !stat_trim_events && !stat_discontinuities) return;
        const double r = rate > 0 ? (double)rate : 48000.0;
        HrLog::Info(std::string("Audio sync (") + who + "): kept on the wall clock - " +
                    std::to_string(stat_pad_events) + " gap(s) filled with " +
                    std::to_string((long long)(stat_padded_frames * 1000.0 / r)) + " ms of silence, " +
                    std::to_string(stat_trim_events) + " surplus trim(s) of " +
                    std::to_string((long long)(stat_trimmed_frames * 1000.0 / r)) + " ms, " +
                    std::to_string(stat_discontinuities) + " WASAPI discontinuity flag(s).");
    }

    // 2.4: per-process loopback (a browser / one window's sound) - defined in audio_extra.inc
    bool open_process(unsigned long pid);

    bool open(bool is_loopback, IMMDevice* dev)
    {
        loopback = is_loopback;
        device   = dev;
        device->AddRef();

        // (handle/COM leak on any Start() that fails partway):
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
        // 2 s of buffer (units of 100 ns). The value used to be 2000000 = only 200 ms: any stall of the
        // capture thread longer than that (CPU at 100%, a game hogging the cores) made WASAPI overwrite
        // unread samples, and the recording silently lost that audio -> A/V drift.
        hr = client->Initialize(mode, flags,
                                20000000LL, 0, mix_fmt, nullptr);
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
            if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) ++stat_discontinuities;

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

        // A/V SYNC FIX (system audio "runs ahead" of the video): WASAPI loopback delivers NO
        // packets at all while nothing is playing - the silence is simply absent, not sent as
        // zeros.
        // A/V SYNC FIX 2: the same wall-clock bookkeeping now runs for EVERY stream, not only for the
        // system-audio loopback. A microphone has no "no packets while silent" case, but its samples are
        // lost when the capture thread is starved or WASAPI overruns, and it used to get no correction
        // at all: the mic track came out shorter than the video, so everything after the first stall
        // sat early or late against the picture. Zeros are inserted where samples are missing, and
        // surplus samples (a device clock running fast) are dropped, so the audio length keeps
        // matching the video length - which is paced by the same QueryPerformanceCounter.
        if (rate > 0 && clock_freq.QuadPart > 0) {   // no clock -> no correction (never trim blindly)
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
            int64_t new_kept = frames_new;
            if (deficit > (int64_t)rate / 20) {
                out.insert(out.begin() + (ptrdiff_t)start_idx, (size_t)deficit * 2, (int16_t)0);
                frames_total += deficit;
                total += (int)(deficit * 2);
                stat_padded_frames += deficit;
                ++stat_pad_events;
            } else if (-deficit > (int64_t)rate / 4 && frames_new > 0) {
                // More audio than wall-clock time (fast device clock): drop the surplus from the end of
                // this read, keeping ~20 ms of headroom so it doesn't trim again on the very next read.
                int64_t trim = -deficit - (int64_t)rate / 50;
                if (trim > frames_new) trim = frames_new;
                if (trim > 0) {
                    out.resize(out.size() - (size_t)trim * 2);
                    total -= (int)(trim * 2);
                    new_kept -= trim;
                    stat_trimmed_frames += trim;
                    ++stat_trim_events;
                }
            }
            frames_total += new_kept;
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
    PcmRing              mic_buf;      // lock: mic_mutex. Ring, not a vector: no realloc-copy, no front-erase memmove
    std::vector<int16_t> mic_scratch;  // flush/stop staging, reused (lock: stream_mutex)
    std::mutex          mic_mutex;
    std::atomic<int>    mic_level{0};

    // Sys
    WasapiStream        sys_stream;
    std::thread         sys_thread;
    PcmRing              sys_buf;      // lock: sys_mutex
    std::vector<int16_t> sys_scratch;  // lock: stream_mutex
    std::mutex          sys_mutex;
    std::atomic<int>    sys_level{0};

    // Control
    std::atomic<bool>   running{false};
    std::atomic<bool>   paused{false};

    std::atomic<bool>   buffering{false};
    std::atomic<int>    max_buffer_sec{0};
    std::mutex stream_mutex;
    FILE*    mic_stream_file  = nullptr;
    FILE*    sys_stream_file  = nullptr;
    uint64_t mic_stream_bytes = 0;
    uint64_t sys_stream_bytes = 0;

    // Volume/mute (written from the UI thread, read from the audio threads)
    std::atomic<float>  mic_vol{1.0f};
    std::atomic<float>  sys_vol{1.0f};
    std::atomic<bool>   mic_mute{false};
    std::atomic<bool>   sys_mute{false};
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
    std::vector<int16_t> tmp;          // reused every iteration (was a fresh vector per 10 ms)
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
        tmp.clear();                       // keeps capacity: no allocation per 10 ms chunk
        st->mic_stream.read(tmp);

        if (!tmp.empty()) {
            float target_vol = st->mic_vol.load();
            bool  mute = st->mic_mute.load();
            if (mute) {
                memset(tmp.data(), 0, tmp.size() * 2);
                st->mic_vol_smooth = target_vol; // don't ramp *into* a stale value on unmute
            } else if (target_vol != st->mic_vol_smooth) {
                // Ramp linearly from the gain applied to the previous chunk's last sample up to wherever
                // the slider is *now*, one step per sample, instead of one flat multiply for the whole
                // chunk.
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
                const int cap_sec = st->max_buffer_sec.load();
                std::lock_guard<std::mutex> lk(st->mic_mutex);
                // Instant Replay window: the ring drops the oldest samples itself (index move, no memmove).
                st->mic_buf.set_limit((cap_sec > 0 && st->mic_stream.rate > 0)
                                         ? (size_t)cap_sec * st->mic_stream.rate * 2   // stereo int16
                                         : 0);
                st->mic_buf.push(tmp.data(), tmp.size());
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
    std::vector<int16_t> tmp;          // reused every iteration (was a fresh vector per 10 ms)
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
        tmp.clear();                       // keeps capacity: no allocation per 10 ms chunk
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
                const int cap_sec = st->max_buffer_sec.load();
                std::lock_guard<std::mutex> lk(st->sys_mutex);
                // Instant Replay window: the ring drops the oldest samples itself (index move, no memmove).
                st->sys_buf.set_limit((cap_sec > 0 && st->sys_stream.rate > 0)
                                         ? (size_t)cap_sec * st->sys_stream.rate * 2   // stereo int16
                                         : 0);
                st->sys_buf.push(tmp.data(), tmp.size());
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

// hr_audio_start(mic_vol, sys_vol, mic_mute, sys_mute, mic_device_id)
// Открывает потоки и запускает потоки записи.
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

static void extra_on_pause(bool paused);   // audio_extra.inc

HR_EXPORT void hr_audio_pause(int paused)
{
    if (g_state) g_state->paused.store(paused != 0);
    extra_on_pause(paused != 0);
}

static void live_shutdown();                // end of this file

HR_EXPORT int hr_audio_stop(const char* mic_wav_path,
                             const char* sys_wav_path)
{
    if (!g_state) return 0;
    live_shutdown();   // the live mixer thread reads g_state: it must be gone before g_state is
    g_state->running.store(false);

    // Join the capture threads BEFORE releasing the WASAPI objects they're using.
    if (g_state->mic_thread.joinable()) g_state->mic_thread.join();
    if (g_state->sys_thread.joinable()) g_state->sys_thread.join();

    g_state->mic_stream.close();
    g_state->sys_stream.close();

    int result = 0;

    if (mic_wav_path) {
        std::lock_guard<std::mutex> lk(g_state->mic_mutex);
        g_state->mic_buf.drain(g_state->mic_scratch);
        if (!g_state->mic_scratch.empty() &&
            wav_write(mic_wav_path, g_state->mic_scratch, 2, g_state->mic_stream.rate))
            result |= 0x1;
    }
    if (sys_wav_path) {
        std::lock_guard<std::mutex> lk(g_state->sys_mutex);
        g_state->sys_buf.drain(g_state->sys_scratch);
        if (!g_state->sys_scratch.empty() &&
            wav_write(sys_wav_path, g_state->sys_scratch, 2, g_state->sys_stream.rate))
            result |= 0x2;
    }

    delete g_state;
    g_state = nullptr;
    return result;
}

// hr_audio_reset_buffers()
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
    g_state->mic_stream.reset_stats();
    g_state->sys_stream.reset_stats();
    g_state->mic_stream_file = wav_stream_open(mic_wav_path);
    g_state->sys_stream_file = wav_stream_open(sys_wav_path);
    // Start buffering PCM again now that an actual recording is underway
    // - see AudioState::buffering's comment for why this was off.
    g_state->buffering.store(true);
}

// hr_audio_flush_buffered()
static void extra_flush();                 // audio_extra.inc

HR_EXPORT void hr_audio_flush_buffered()
{
    extra_flush();   // 2.4: extra mixer channels stream to disk on the same tick
    if (!g_state) return;
    std::lock_guard<std::mutex> slk(g_state->stream_mutex);
    if (g_state->mic_stream_file) {
        std::vector<int16_t> &chunk = g_state->mic_scratch;   // reused: no allocation per flush
        {
            std::lock_guard<std::mutex> lk(g_state->mic_mutex);
            g_state->mic_buf.drain(chunk);
        }
        if (!chunk.empty()) {
            wav_stream_append(g_state->mic_stream_file, chunk.data(), chunk.size());
            g_state->mic_stream_bytes += chunk.size() * 2;
        }
    }
    if (g_state->sys_stream_file) {
        std::vector<int16_t> &chunk = g_state->sys_scratch;   // reused: no allocation per flush
        {
            std::lock_guard<std::mutex> lk(g_state->sys_mutex);
            g_state->sys_buf.drain(chunk);
        }
        if (!chunk.empty()) {
            wav_stream_append(g_state->sys_stream_file, chunk.data(), chunk.size());
            g_state->sys_stream_bytes += chunk.size() * 2;
        }
    }
}

// hr_audio_capture_to_wav(mic_wav_path, sys_wav_path)
HR_EXPORT int hr_audio_capture_to_wav(const char* mic_wav_path,
                                        const char* sys_wav_path)
{
    if (!g_state) return 0;
    int result = 0;

    // Stop buffering PCM until the next recording starts - see
    // AudioState::buffering's comment. Set before taking the buffers below so
    // there's no window where a worker thread could sneak in one more insert().
    g_state->buffering.store(false);

    g_state->mic_stream.log_stats("mic");
    g_state->sys_stream.log_stats("system");

    hr_audio_flush_buffered();

    std::lock_guard<std::mutex> slk(g_state->stream_mutex);
    if (g_state->mic_stream_file) {
        // Streaming was open for this recording (hr_audio_reset_buffers()
        wav_stream_close(g_state->mic_stream_file, 2, g_state->mic_stream.rate, g_state->mic_stream_bytes);
        g_state->mic_stream_file = nullptr;
        if (mic_wav_path && g_state->mic_stream_bytes > 0) result |= 0x1;
        g_state->mic_stream_bytes = 0;
    } else if (mic_wav_path) {
        // Fallback for a caller that never opened streaming (e.g.
        std::vector<int16_t> &mic_out = g_state->mic_scratch;
        {
            std::lock_guard<std::mutex> lk(g_state->mic_mutex);
            g_state->mic_buf.drain(mic_out);
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
        std::vector<int16_t> &sys_out = g_state->sys_scratch;
        {
            std::lock_guard<std::mutex> lk(g_state->sys_mutex);
            g_state->sys_buf.drain(sys_out);
        }
        if (!sys_out.empty() && wav_write(sys_wav_path, sys_out, 2, g_state->sys_stream.rate))
            result |= 0x2;
    }
    return result;
}

// hr_audio_set_max_buffer_sec(seconds)
HR_EXPORT void hr_audio_set_max_buffer_sec(int seconds)
{
    if (!g_state) return;
    g_state->max_buffer_sec.store(seconds > 0 ? seconds : 0);
}

// hr_audio_snapshot_to_wav(mic_wav_path, sys_wav_path)
// Number of newest samples a rolling buffer exposes: the last max_buffer_sec seconds (0 = all).
// Always even, so stereo frames stay aligned.
static size_t window_samples(int cap_sec, int rate)
{
    if (cap_sec <= 0 || rate <= 0) return 0;
    return (size_t)cap_sec * (size_t)rate * 2;
}

// The same snapshot, but of the NEWEST `window_ms` milliseconds (window_ms <= 0 = the whole rolling
// buffer, i.e. hr_audio_snapshot_to_wav()). The RAM Instant Replay knows exactly how long the video it saved is,
// so the audio is cut to that length at the same (newest) end - before, the audio window was up to 5-15 s longer
// than the video and, merged from t=0, ran that much ahead of the picture once the buffer was full.
HR_EXPORT int hr_audio_snapshot_to_wav_ex(const char* mic_wav_path,
                                            const char* sys_wav_path,
                                            int window_ms)
{
    if (!g_state) return 0;
    int result = 0;
    // PERF: copy the window under the lock, write to disk after releasing it,
    // so saving a replay no longer stalls the capture threads (audible as a
    // glitch in the still-running buffer) for the duration of the file write.
    const int cap_sec = g_state->max_buffer_sec.load();
    std::vector<int16_t> mic_copy, sys_copy;
    int mic_rate = 0, sys_rate = 0;
    // samples (stereo int16) = ms * rate * 2 / 1000, never more than the rolling window itself
    auto want = [&](int rate) -> size_t {
        const size_t full = window_samples(cap_sec, rate);
        if (window_ms <= 0 || rate <= 0) return full;
        size_t n = (size_t)((long long)window_ms * rate / 1000) * 2;
        if (full > 0 && n > full) n = full;
        return n;
    };
    if (mic_wav_path) {
        std::lock_guard<std::mutex> lk(g_state->mic_mutex);
        mic_rate = g_state->mic_stream.rate;
        g_state->mic_buf.copy_tail(mic_copy, want(mic_rate));
    }
    if (sys_wav_path) {
        std::lock_guard<std::mutex> lk(g_state->sys_mutex);
        sys_rate = g_state->sys_stream.rate;
        g_state->sys_buf.copy_tail(sys_copy, want(sys_rate));
    }
    if (!mic_copy.empty() && wav_write(mic_wav_path, mic_copy, 2, mic_rate)) result |= 0x1;
    if (!sys_copy.empty() && wav_write(sys_wav_path, sys_copy, 2, sys_rate)) result |= 0x2;
    return result;
}

HR_EXPORT int hr_audio_snapshot_to_wav(const char* mic_wav_path,
                                         const char* sys_wav_path)
{
    return hr_audio_snapshot_to_wav_ex(mic_wav_path, sys_wav_path, 0);
}

// mix_wav_inmemory(mic_wav, sys_wav, out_wav)
// The ORIGINAL two-file mixer: reads both inputs whole and builds a third buffer, i.e. it
// needs ~3x the track size in RAM (about 1 GB for a 30-minute recording, 2 GB for an hour).
static int mix_wav_inmemory(const char* mic_path,
                            const char* sys_path,
                            const char* out_path)
{
    std::vector<int16_t> mic_pcm, sys_pcm;
    uint16_t mic_ch = 2, sys_ch = 2;
    uint32_t mic_rate = 44100, sys_rate = 44100;

    if (!wav_read(mic_path, mic_pcm, mic_ch, mic_rate)) return -1;
    if (!wav_read(sys_path, sys_pcm, sys_ch, sys_rate)) return -2;
    // Never "succeed" with an empty result: the caller deletes the source WAVs after a successful mix.
    if (mic_pcm.empty() && sys_pcm.empty()) return -4;

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

    // The two WAVs now (post-fix) carry each stream's real native capture rate, which the mic
    // and system-output endpoints are free to differ on.
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

// hr_audio_mix_wav_list(inputs, out_path)   (2.4)
// Mixes any number of 16-bit stereo WAVs (this file's own writers: 44-byte header)
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
        // ftell() is 32-bit on Windows (-1 above 2 GB) and the header's size field wraps at 4 GiB, so the
        // length comes from the 64-bit file size whenever the header can't be trusted (see wav_util.h).
        uint64_t fsz = 0;
        if (!hrwav::FileSize64(f, fsz) || fsz < sizeof(h)) { fclose(f); f = nullptr; return false; }
        const uint64_t bytes = hrwav::EffectiveDataBytes(h.data_size, fsz - sizeof(h));
        total = bytes / 4;
        if (total == 0) { fclose(f); f = nullptr; }
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

// hr_audio_mix_wav(mic_wav, sys_wav, out_wav)
// Смешивает два WAV файла в один (без normalize, без subprocess).
// Возвращает 0 при успехе.
HR_EXPORT int hr_audio_mix_wav(const char* mic_path,
                                const char* sys_path,
                                const char* out_path)
{
    if (!mic_path || !sys_path || !out_path) return -1;
    // The streaming mixer silently skips an input it can't read, which here would drop a whole
    // track - so only use it when BOTH files are plain 16-bit stereo WAVs (what this app writes).
    auto stereo16 = [](const char* path) -> bool {
        FILE* f = fopen(path, "rb");
        if (!f) return false;
        WavHeader h;
        const bool ok = fread(&h, sizeof(h), 1, f) == 1 && h.num_channels == 2 && h.bits_per_smp == 16;
        fclose(f);
        return ok;
    };
    if (stereo16(mic_path) && stereo16(sys_path)) {
        std::string inputs = std::string(mic_path) + "|" + sys_path;
        if (hr_audio_mix_wav_list(inputs.c_str(), out_path) == 0) return 0;
    }
    // Streaming mixer refused the files (unexpected header / channel count): old behaviour.
    return mix_wav_inmemory(mic_path, sys_path, out_path);
}

/*  hr_audio_rms(buf, n_bytes)  - быстрый RMS для уже захваченного буфера */
HR_EXPORT int hr_audio_rms(const void* buf, int n_bytes)
{
    return calc_rms((const int16_t*)buf, n_bytes / 2);
}

// 2.4: extra Audio Mixer channels (another mic, a browser / window, an audio file)
#include "audio_extra.inc"

// ===========================================================================
// In-process audio: live mixing of mic + system + extra channels
// ===========================================================================
// Instead of streaming every source into its own WAV and mixing / merging after Stop, a mixer thread drains
// the same capture rings every 20 ms, mixes them (audio_mixer.h) and hands the result to a SINK while the
// recording is running:
//   * the Media Foundation sink writer (mf_encoder.cpp: hr_mf_write_audio) - AAC encoded + muxed in process;
//   * a named pipe that ffmpeg reads as a second input and muxes live (audio_pipe.cpp: hr_apipe_write).
// Volume / mute / level meters / the wall-clock padding stay exactly where they were (capture workers).
//
// Call order (RecordingController::Start): hr_audio_live_prepare() -> [build the sink] -> hr_audio_pause(0),
// hr_audio_reset_buffers(nullptr, nullptr), hr_audio_set_volumes(), hr_audio_extra_begin_live() ->
// hr_audio_live_start(). Stop: hr_audio_live_stop() BEFORE the video side is finalized.

typedef void (*HrAudioSinkFn)(void *user, const int16_t *stereo, size_t frames);

namespace {

// An audio-file channel (Audio Mixer kind 2): read from the start of the file, at the pace the mixer asks.
struct FilePull {
    int      id = 0;
    uint64_t next_in = 0;                  // next frame of the file to read
    hrlive::StereoResampler rs;
    std::vector<int16_t> ready;            // resampled, not yet consumed
    size_t   ready_rd = 0;
    std::vector<int16_t> raw;
    bool     eof = false;
};

void file_pull(void *ctx, int32_t *acc, size_t frames)
{
    FilePull *c = static_cast<FilePull *>(ctx);
    std::lock_guard<std::mutex> lk(hrx::g_extras_mu);      // the meter (hr_audio_extra_level) shares the FILE*
    hrx::Extra *s = hrx::find_extra_locked(c->id);
    if (!s || !s->rf) return;                               // removed meanwhile: silence
    const float g = s->mute.load() ? 0.0f : s->vol.load();
    const uint64_t total = s->data_bytes / 4;

    while (!c->eof && (c->ready.size() - c->ready_rd) / 2 < frames) {
        if (c->next_in >= total) { c->eof = true; break; }
        const size_t want = (size_t)std::min<uint64_t>(2048, total - c->next_in);
        c->raw.resize(want * 2);
        if (_fseeki64(s->rf, (long long)s->data_off + (long long)(c->next_in * 4), SEEK_SET) != 0) { c->eof = true; break; }
        const size_t got = fread(c->raw.data(), 4, want, s->rf);
        if (got == 0) { c->eof = true; break; }
        c->next_in += got;
        c->rs.process(c->raw.data(), got, c->ready);
    }
    const size_t have = (c->ready.size() - c->ready_rd) / 2;
    const size_t take = std::min(have, frames);
    const int16_t *p = c->ready.data() + c->ready_rd;
    if (g == 1.0f) { for (size_t i = 0; i < take * 2; ++i) acc[i] += p[i]; }
    else if (g != 0.0f) { for (size_t i = 0; i < take * 2; ++i) acc[i] += (int32_t)((float)p[i] * g); }
    c->ready_rd += take * 2;
    if (c->ready_rd >= c->ready.size()) { c->ready.clear(); c->ready_rd = 0; }
}

struct LiveSession {
    hrlive::LiveMixer mixer;
    std::thread       th;
    HANDLE            stop_ev = nullptr;
    HrAudioSinkFn     sink = nullptr;
    void             *user = nullptr;
    uint32_t          rate = 48000;
    int               mic_idx = -1, sys_idx = -1;
    std::vector<std::pair<int, int>>        extra_idx;     // (extra id, mixer source index)
    std::vector<std::unique_ptr<FilePull>>  files;
    std::vector<int16_t> scratch, out;
    ~LiveSession() { if (stop_ev) CloseHandle(stop_ev); }
};

LiveSession *g_live = nullptr;

void live_tick(LiveSession *L, bool final_flush)
{
    AudioState *st = g_state;
    if (!st) return;
    if (L->mic_idx >= 0) {
        { std::lock_guard<std::mutex> lk(st->mic_mutex); st->mic_buf.drain(L->scratch); }
        if (!L->scratch.empty()) L->mixer.push(L->mic_idx, L->scratch.data(), L->scratch.size());
    }
    if (L->sys_idx >= 0) {
        { std::lock_guard<std::mutex> lk(st->sys_mutex); st->sys_buf.drain(L->scratch); }
        if (!L->scratch.empty()) L->mixer.push(L->sys_idx, L->scratch.data(), L->scratch.size());
    }
    if (!L->extra_idx.empty()) {
        std::lock_guard<std::mutex> lk(hrx::g_extras_mu);   // same order as extra_flush(): g_extras_mu -> Extra::mu
        for (const auto &e : L->extra_idx) {
            hrx::Extra *s = hrx::find_extra_locked(e.first);
            if (!s) continue;                               // removed while recording: its source just stays silent
            { std::lock_guard<std::mutex> bl(s->mu); s->buf.drain(L->scratch); }
            if (!L->scratch.empty()) L->mixer.push(e.second, L->scratch.data(), L->scratch.size());
        }
    }
    L->out.clear();
    const size_t n = L->mixer.mix(L->out, final_flush);
    if (n && L->sink) L->sink(L->user, L->out.data(), n);
}

void live_thread(LiveSession *L)
{
    try {
        while (WaitForSingleObject(L->stop_ev, 20) == WAIT_TIMEOUT) live_tick(L, false);
        live_tick(L, true);                                 // everything still buffered goes out
    } catch (const std::exception &e) {
        HrLog::Error(std::string("Audio live mixer: uncaught exception (") + e.what() + ")");
    } catch (...) {
        HrLog::Error("Audio live mixer: uncaught unknown exception");
    }
}

// Ends the session (if any) and returns the frames it produced.
long long live_end()
{
    LiveSession *L = g_live;
    if (!L) return 0;
    g_live = nullptr;
    SetEvent(L->stop_ev);
    if (L->th.joinable()) L->th.join();
    const long long frames = (long long)L->mixer.frames_out();
    if (L->mixer.frames_padded() || L->mixer.frames_dropped()) {
        const double r = L->rate ? (double)L->rate : 48000.0;
        HrLog::Info("Audio live mixer: a source fell behind - " +
                    std::to_string((long long)(L->mixer.frames_padded() * 1000.0 / r)) + " ms of silence inserted, " +
                    std::to_string((long long)(L->mixer.frames_dropped() * 1000.0 / r)) + " ms of late audio dropped.");
    }
    delete L;
    return frames;
}

} // namespace

static void live_shutdown() { live_end(); }

// Rate (Hz) the live mix will have, or 0 when nothing can be captured (then the caller uses the WAV path).
// All sources on the same 44.1 / 48 kHz -> that rate (no resampling); otherwise 48 kHz.
HR_EXPORT int hr_audio_live_prepare()
{
    if (!g_state) return 0;
    std::vector<uint32_t> rates;
    if (g_state->mic_thread.joinable() && g_state->mic_stream.rate) rates.push_back(g_state->mic_stream.rate);
    if (g_state->sys_thread.joinable() && g_state->sys_stream.rate) rates.push_back(g_state->sys_stream.rate);
    {
        std::lock_guard<std::mutex> lk(hrx::g_extras_mu);
        for (auto &p : hrx::g_extras)
            if (p->kind != 2 && p->ok.load() && p->run.load() && p->stream.rate) rates.push_back(p->stream.rate);
    }
    if (rates.empty()) return 0;
    bool same = true;
    for (uint32_t r : rates) if (r != rates[0]) same = false;
    if (same && (rates[0] == 44100 || rates[0] == 48000)) return (int)rates[0];
    return 48000;
}

// Starts mixing. out_rate: what hr_audio_live_prepare() returned (the sink was built for it). head_ms: how much
// LATER than the video's first frame the audio started (>0: silence is put in front; <0: that much is cut).
// Returns 1 on success.
HR_EXPORT int hr_audio_live_start(HrAudioSinkFn sink, void *user, int out_rate, int head_ms)
{
    if (!g_state || g_live || !sink) return 0;
    try {
        auto L = std::make_unique<LiveSession>();
        L->rate = (out_rate == 44100 || out_rate == 48000) ? (uint32_t)out_rate : 48000u;
        L->mixer.init(L->rate);
        if (g_state->mic_thread.joinable() && g_state->mic_stream.rate)
            L->mic_idx = L->mixer.add_push(g_state->mic_stream.rate);
        if (g_state->sys_thread.joinable() && g_state->sys_stream.rate)
            L->sys_idx = L->mixer.add_push(g_state->sys_stream.rate);
        {
            std::lock_guard<std::mutex> lk(hrx::g_extras_mu);
            for (auto &p : hrx::g_extras) {
                if (p->kind != 2) {
                    // buffering == hr_audio_extra_begin_live() ran for this channel (channels added later stay out, as before).
                    if (p->ok.load() && p->run.load() && p->buffering.load() && p->stream.rate)
                        L->extra_idx.emplace_back(p->id, L->mixer.add_push(p->stream.rate));
                } else if (p->rf && hrx::g_extra_recording.load()) {
                    auto f = std::make_unique<FilePull>();
                    f->id = p->id;
                    f->rs.init(p->file_rate ? p->file_rate : 44100, L->rate);
                    L->mixer.add_pull(file_pull, f.get());
                    L->files.push_back(std::move(f));
                }
            }
        }
        if (L->mixer.push_sources() == 0) return 0;
        L->mixer.set_head((int64_t)head_ms * (int64_t)L->rate / 1000);
        L->sink = sink;
        L->user = user;
        L->stop_ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!L->stop_ev) return 0;
        L->th = std::thread(live_thread, L.get());
        g_live = L.release();
        return 1;
    } catch (const std::exception &e) {
        HrLog::Error(std::string("Audio live mixer: could not start (") + e.what() + ")");
    } catch (...) {
        HrLog::Error("Audio live mixer: could not start (unknown exception)");
    }
    return 0;
}

HR_EXPORT int hr_audio_live_active() { return g_live ? 1 : 0; }

// Ends the live mix: the last buffered audio is mixed and handed to the sink, then buffering stops (the
// counterpart of hr_audio_capture_to_wav()). Returns how many frames were produced.
HR_EXPORT long long hr_audio_live_stop()
{
    const long long frames = live_end();
    if (g_state) {
        g_state->buffering.store(false);
        g_state->mic_stream.log_stats("mic");
        g_state->sys_stream.log_stats("system");
        { std::lock_guard<std::mutex> lk(g_state->mic_mutex); g_state->mic_buf.clear(); }
        { std::lock_guard<std::mutex> lk(g_state->sys_mutex); g_state->sys_buf.clear(); }
    }
    hr_audio_extra_end_live();
    return frames;
}
