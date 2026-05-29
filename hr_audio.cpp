
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devcpd.h>
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
    return std::min(100, (int)(rms / 150.0));
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
        DWORD flags = is_loopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0;
        hr = client->Initialize(mode, flags,
                                2000000LL, 0, mix_fmt, nullptr);
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

    // Volume/mute (written from Python, read from C++ threads)
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
    // Sleep interval: ~10ms for smooth read
    const int SLEEP_MS = 10;
    while (st->running.load()) {
        if (st->paused.load()) {
            Sleep(SLEEP_MS);
            continue;
        }
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
            std::lock_guard<std::mutex> lk(st->mic_mutex);
            st->mic_buf.insert(st->mic_buf.end(), tmp.begin(), tmp.end());
        }
        Sleep(SLEEP_MS);
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
            std::lock_guard<std::mutex> lk(st->sys_mutex);
            st->sys_buf.insert(st->sys_buf.end(), tmp.begin(), tmp.end());
        }
        Sleep(SLEEP_MS);
    }
}

// ---------------------------------------------------------------------------
// Public C API
// ---------------------------------------------------------------------------

/*  hr_audio_init()
    Вызвать один раз при старте.
    Возвращает 0 при успехе, отрицательное — ошибка CoInitialize. */
HR_EXPORT int hr_audio_init()
{
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) return -1;
    return 0;
}

/*  hr_audio_start(mic_vol, sys_vol, mic_mute, sys_mute)
    Открывает потоки и запускает потоки записи.
    Возвращает битовую маску: bit0=mic_ok, bit1=sys_ok. */
HR_EXPORT int hr_audio_start(float mic_vol, float sys_vol,
                              int mic_mute, int sys_mute)
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
    IMMDevice* mic_dev = get_default_input(enumerator);
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

/*  hr_audio_set_volumes(mic_vol, sys_vol, mic_mute, sys_mute)
    Потокобезопасно меняет громкость/мут в реальном времени. */
HR_EXPORT void hr_audio_set_volumes(float mic_vol, float sys_vol,
                                     int mic_mute, int sys_mute)
{
    if (!g_state) return;
    g_state->mic_vol.store(mic_vol);
    g_state->sys_vol.store(sys_vol);
    g_state->mic_mute.store(mic_mute != 0);
    g_state->sys_mute.store(sys_mute != 0);
}

/*  hr_audio_get_levels(out_mic, out_sys)
    Записывает текущие уровни VU (0-100) в переданные указатели. */
HR_EXPORT void hr_audio_get_levels(int* out_mic, int* out_sys)
{
    if (!g_state) { if(out_mic) *out_mic=0; if(out_sys) *out_sys=0; return; }
    if (out_mic) *out_mic = g_state->mic_level.load();
    if (out_sys) *out_sys = g_state->sys_level.load();
}

/*  hr_audio_pause(paused)  — 1 = пауза, 0 = продолжить */
HR_EXPORT void hr_audio_pause(int paused)
{
    if (g_state) g_state->paused.store(paused != 0);
}

/*  hr_audio_stop(mic_wav_path, sys_wav_path)
    Останавливает запись, сбрасывает буферы в WAV-файлы.
    Передайте nullptr если файл не нужен.
    Возвращает: bit0 = mic_wav записан, bit1 = sys_wav записан. */
HR_EXPORT int hr_audio_stop(const char* mic_wav_path,
                             const char* sys_wav_path)
{
    if (!g_state) return 0;
    g_state->running.store(false);

    g_state->mic_stream.close();
    g_state->sys_stream.close();

    if (g_state->mic_thread.joinable()) g_state->mic_thread.join();
    if (g_state->sys_thread.joinable()) g_state->sys_thread.join();

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

/*  hr_audio_rms(buf, n_bytes)  — быстрый RMS для уже захваченного буфера */
HR_EXPORT int hr_audio_rms(const void* buf, int n_bytes)
{
    return calc_rms((const int16_t*)buf, n_bytes / 2);
}
