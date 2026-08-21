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

    bool open(bool is_loopback, IMMDevice* dev)
    {
        loopback = is_loopback;
        device   = dev;
        device->AddRef();

        HRESULT hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL,
                                      nullptr, (void**)&client);
        if (FAILED(hr)) return false;

        hr = client->GetMixFormat(&mix_fmt);
        if (FAILED(hr)) return false;

        channels = (uint16_t)mix_fmt->nChannels;
        rate     = mix_fmt->nSamplesPerSec;

        AUDCLNT_SHAREMODE mode = AUDCLNT_SHAREMODE_SHARED;
        DWORD flags = (is_loopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0)
                      | AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
        hr = client->Initialize(mode, flags,
                                2000000LL, 0, mix_fmt, nullptr);
        if (FAILED(hr)) return false;

        data_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!data_event) return false;
        hr = client->SetEventHandle(data_event);
        if (FAILED(hr)) return false;

        hr = client->GetService(__uuidof(IAudioCaptureClient),
                                (void**)&capture);
        if (FAILED(hr)) return false;

        hr = client->Start();
        return SUCCEEDED(hr);
    }

    // Read available frames, convert to int16 stereo 44100
    // Returns number of int16 samples written to out (interleaved)
    int read(std::vector<int16_t>& out)
    {
        if (!capture) return 0;
        int total = 0;
        UINT32 pkt = 0;
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

    // BUGFIX (AFK memory growth): mic_buf/sys_buf below used to be
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

    // Volume/mute (written from the UI thread, read from the audio threads)
    std::atomic<float>  mic_vol{1.0f};
    std::atomic<float>  sys_vol{1.0f};
    std::atomic<bool>   mic_mute{false};
    std::atomic<bool>   sys_mute{false};
};

static AudioState* g_state = nullptr;

// ---------------------------------------------------------------------------
// Capture threads
// ---------------------------------------------------------------------------
static void mic_worker(AudioState* st)
{
    const int SLEEP_MS = 10;
    while (st->running.load()) {
        if (st->paused.load()) {
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
            float vol  = st->mic_vol.load();
            bool  mute = st->mic_mute.load();
            if (mute) {
                memset(tmp.data(), 0, tmp.size() * 2);
            } else if (vol != 1.0f) {
                for (auto& s : tmp) {
                    int v = (int)(s * vol);
                    s = (int16_t)std::max(-32768, std::min(32767, v));
                }
            }
            st->mic_level.store(mute ? 0 : calc_rms(tmp.data(), tmp.size()));
            if (st->buffering.load()) {
                std::lock_guard<std::mutex> lk(st->mic_mutex);
                st->mic_buf.insert(st->mic_buf.end(), tmp.begin(), tmp.end());
            }
        }
        // No trailing Sleep here anymore -- the wait at the top of the loop
        // (event or fallback poll) already paces this thread; sleeping again
        // here would just add a second, redundant delay on top of it.
    }
}

static void sys_worker(AudioState* st)
{
    const int SLEEP_MS = 10;
    while (st->running.load()) {
        if (st->paused.load()) {
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
            float vol  = st->sys_vol.load();
            bool  mute = st->sys_mute.load();
            if (mute) {
                memset(tmp.data(), 0, tmp.size() * 2);
            } else if (vol != 1.0f) {
                for (auto& s : tmp) {
                    int v = (int)(s * vol);
                    s = (int16_t)std::max(-32768, std::min(32767, v));
                }
            }
            st->sys_level.store(mute ? 0 : calc_rms(tmp.data(), tmp.size()));
            if (st->buffering.load()) {
                std::lock_guard<std::mutex> lk(st->sys_mutex);
                st->sys_buf.insert(st->sys_buf.end(), tmp.begin(), tmp.end());
            }
        }
        // See matching comment in mic_worker() above.
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
    // BUGFIX: this always opened whatever Windows considered the default
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

HR_EXPORT void hr_audio_pause(int paused)
{
    if (g_state) g_state->paused.store(paused != 0);
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
            wav_write(mic_wav_path, g_state->mic_buf, 2, 44100))
            result |= 0x1;
    }
    if (sys_wav_path) {
        std::lock_guard<std::mutex> lk(g_state->sys_mutex);
        if (!g_state->sys_buf.empty() &&
            wav_write(sys_wav_path, g_state->sys_buf, 2, 44100))
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
HR_EXPORT void hr_audio_reset_buffers()
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
    // Start buffering PCM again now that an actual recording is underway
    // - see AudioState::buffering's comment for why this was off.
    g_state->buffering.store(true);
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
    // AudioState::buffering's comment. Set before the writes below so
    // there's no window where a worker thread could sneak in one more
    // insert() between the wav_write() and clear() for its stream.
    g_state->buffering.store(false);

    {
        std::lock_guard<std::mutex> lk(g_state->mic_mutex);
        if (mic_wav_path && !g_state->mic_buf.empty() &&
            wav_write(mic_wav_path, g_state->mic_buf, 2, 44100))
            result |= 0x1;
        g_state->mic_buf.clear();
    }
    {
        std::lock_guard<std::mutex> lk(g_state->sys_mutex);
        if (sys_wav_path && !g_state->sys_buf.empty() &&
            wav_write(sys_wav_path, g_state->sys_buf, 2, 44100))
            result |= 0x2;
        g_state->sys_buf.clear();
    }

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

    return wav_write(out_path, out, 2, 44100) ? 0 : -3;
}

/*  hr_audio_rms(buf, n_bytes)  - быстрый RMS для уже захваченного буфера */
HR_EXPORT int hr_audio_rms(const void* buf, int n_bytes)
{
    return calc_rms((const int16_t*)buf, n_bytes / 2);
}