// audio_extra.inc  (2.4)  -  extra Audio Mixer channels.
//
// #included at the very end of audio.cpp so it can share that file's statics
// (WasapiStream, calc_rms, the WAV helpers). Not a separate translation unit on
// purpose: nothing new to add to the Makefile/CMake source lists.
//
// A channel ("extra source") is one of:
//   kind 0  another microphone / input device          (WASAPI capture of that endpoint)
//   kind 1  one program's sound - a browser or a window (WASAPI per-process loopback,
//                                                         Windows 10 2004+)
//   kind 2  an audio file                                (already decoded to a 44.1kHz
//                                                         stereo WAV by the caller; it is
//                                                         mixed into the recording from t=0)
//
// While a recording runs, kinds 0/1 stream to "<stem>_x<id>.wav" exactly like the
// built-in mic/system tracks do (RAM stays flat). When the recording stops,
// hr_audio_extra_collect() finishes those files, renders the file sources, and hands
// the list back so RecordingController can mix them into the final audio track.
//
// Cost when nothing is added: zero threads, zero allocations.

#include <memory>

namespace hrx {

// ---- per-process loopback activation (declared by hand: older MinGW headers lack it) -----
struct ProcLoopbackParams { DWORD TargetProcessId; int ProcessLoopbackMode; };
struct ActivationParams   { int ActivationType; ProcLoopbackParams Params; };
// ActivationType 1 = PROCESS_LOOPBACK, ProcessLoopbackMode 0 = INCLUDE_TARGET_PROCESS_TREE

static const GUID kIidActivateHandler =
    {0x41D949AB, 0x9862, 0x444A, {0x80, 0xF6, 0xC2, 0x61, 0x33, 0x4D, 0xA5, 0xEB}};
static const GUID kIidAgileObject =
    {0x94EA2B94, 0xE9CC, 0x49E0, {0xC0, 0xFF, 0xEE, 0x64, 0xCA, 0x8F, 0x5B, 0x90}};

struct IAsyncOp : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetActivateResult(HRESULT *activateResult, IUnknown **activatedInterface) = 0;
};
struct IHandler : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE ActivateCompleted(IAsyncOp *op) = 0;
};

class Handler final : public IHandler {
public:
    HANDLE        ev     = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HRESULT       hr     = E_FAIL;
    IAudioClient *client = nullptr;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualGUID(riid, IID_IUnknown) || IsEqualGUID(riid, kIidActivateHandler) ||
            IsEqualGUID(riid, kIidAgileObject)) {
            *ppv = static_cast<IHandler *>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)++ref_; }
    ULONG STDMETHODCALLTYPE Release() override {
        long n = --ref_;
        if (n == 0) delete this;
        return (ULONG)n;
    }
    HRESULT STDMETHODCALLTYPE ActivateCompleted(IAsyncOp *op) override {
        HRESULT result = E_FAIL;
        IUnknown *unk = nullptr;
        HRESULT h = op ? op->GetActivateResult(&result, &unk) : E_POINTER;
        if (SUCCEEDED(h) && SUCCEEDED(result) && unk) {
            IAudioClient *c = nullptr;
            if (SUCCEEDED(unk->QueryInterface(__uuidof(IAudioClient), (void **)&c)) && c) {
                client = c;
                hr = S_OK;
            } else {
                hr = E_NOINTERFACE;
            }
        } else {
            hr = FAILED(h) ? h : result;
        }
        if (unk) unk->Release();
        if (ev) SetEvent(ev);
        return S_OK;
    }
private:
    ~Handler() { if (client) client->Release(); if (ev) CloseHandle(ev); }
    std::atomic<long> ref_{1};
};

} // namespace hrx

#ifndef AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
#define AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM 0x80000000
#endif
#ifndef AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY
#define AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY 0x08000000
#endif

// Opens a capture stream that carries ONLY the sound of `pid` (and processes it started).
bool WasapiStream::open_process(unsigned long pid)
{
    loopback = true;   // same silent-gap padding as desktop loopback (no packets while silent)

    using ActivateFn = HRESULT(WINAPI *)(LPCWSTR, REFIID, PROPVARIANT *, hrx::IHandler *, void **);
    HMODULE mm = LoadLibraryW(L"Mmdevapi.dll");
    if (!mm) return false;
    auto fn = reinterpret_cast<ActivateFn>(GetProcAddress(mm, "ActivateAudioInterfaceAsync"));
    if (!fn) return false;

    hrx::ActivationParams ap{};
    ap.ActivationType = 1;
    ap.Params.TargetProcessId = (DWORD)pid;
    ap.Params.ProcessLoopbackMode = 0;

    PROPVARIANT pv;
    PropVariantInit(&pv);
    pv.vt = VT_BLOB;
    pv.blob.cbSize = (ULONG)sizeof(ap);
    pv.blob.pBlobData = reinterpret_cast<BYTE *>(&ap);

    hrx::Handler *h = new hrx::Handler();
    void *op = nullptr;
    HRESULT hr = fn(L"VAD\\Process_Loopback", __uuidof(IAudioClient), &pv, h, &op);
    if (FAILED(hr)) { h->Release(); return false; }
    if (op) static_cast<IUnknown *>(op)->Release();   // the handler already got the result

    // The completion callback runs on a system thread. If it has not fired within the timeout, do
    // NOT look at h->hr / h->client (the callback may be writing them right now) - just give up;
    // the activation operation still owns a reference to the handler, so it stays valid and
    // releases whatever client it receives later.
    if (WaitForSingleObject(h->ev, 4000) != WAIT_OBJECT_0) {
        HrLog::Warn("Audio Mixer: per-process audio capture didn't answer within 4 s (pid " + std::to_string(pid) + ").");
        h->Release();
        return false;
    }
    if (FAILED(h->hr) || !h->client) { h->Release(); return false; }
    client = h->client;
    client->AddRef();
    h->Release();

    // Process loopback can't report a mix format: ask for plain 16-bit stereo and let
    // Windows convert (AUTOCONVERTPCM).
    WAVEFORMATEX *f = (WAVEFORMATEX *)CoTaskMemAlloc(sizeof(WAVEFORMATEX));
    if (!f) { close(); return false; }
    std::memset(f, 0, sizeof(*f));
    f->wFormatTag = WAVE_FORMAT_PCM;
    f->nChannels = 2;
    f->nSamplesPerSec = 44100;
    f->wBitsPerSample = 16;
    f->nBlockAlign = 4;
    f->nAvgBytesPerSec = 44100 * 4;
    mix_fmt = f;
    channels = 2;
    rate = 44100;

    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                            AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                            AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                            2000000LL, 0, mix_fmt, nullptr);
    if (FAILED(hr)) { close(); return false; }

    data_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!data_event) { close(); return false; }
    if (FAILED(client->SetEventHandle(data_event))) { close(); return false; }
    if (FAILED(client->GetService(__uuidof(IAudioCaptureClient), (void **)&capture))) { close(); return false; }
    if (FAILED(client->Start())) { close(); return false; }

    QueryPerformanceFrequency(&clock_freq);
    QueryPerformanceCounter(&clock_t0);
    frames_total = 0;
    clock_started = true;
    return true;
}

namespace hrx {

struct Extra {
    int                 id   = 0;
    int                 kind = 0;
    std::wstring        target;
    unsigned long       pid  = 0;

    std::atomic<float>  vol{1.0f};
    std::atomic<bool>   mute{false};
    std::atomic<int>    level{0};
    std::atomic<bool>   ok{false};
    std::atomic<bool>   run{false};
    std::atomic<bool>   buffering{false};
    float               smooth = 1.0f;      // worker-thread only

    // kinds 0/1
    WasapiStream        stream;
    std::thread         th;
    std::vector<int16_t> buf;
    std::mutex          mu;
    FILE               *out_file = nullptr;
    uint64_t            out_bytes = 0;

    // kind 2
    std::wstring        wav_path;
    FILE               *rf = nullptr;
    long                data_off = 0;
    uint32_t            data_bytes = 0;
    uint32_t            file_rate = 44100;
};

static std::vector<std::unique_ptr<Extra>> g_extras;
static std::mutex                          g_extras_mu;
static int                                 g_extra_next_id = 1;
static std::string                         g_extra_stem;
static std::atomic<bool>                   g_extra_paused{false};
static std::atomic<bool>                   g_extra_recording{false};
static LARGE_INTEGER                       g_extra_t0{};
static LARGE_INTEGER                       g_extra_freq{};
static double                              g_extra_paused_accum = 0.0;
static LARGE_INTEGER                       g_extra_pause_start{};
static bool                                g_extra_in_pause = false;

static double rec_elapsed_sec()
{
    if (!g_extra_recording.load() || g_extra_freq.QuadPart <= 0) return 0.0;
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    double e = (double)(now.QuadPart - g_extra_t0.QuadPart) / (double)g_extra_freq.QuadPart - g_extra_paused_accum;
    if (g_extra_in_pause)
        e -= (double)(now.QuadPart - g_extra_pause_start.QuadPart) / (double)g_extra_freq.QuadPart;
    return e < 0.0 ? 0.0 : e;
}

// Same linear gain ramp the built-in mic/system workers use (no zipper noise on fader moves).
static void apply_gain(std::vector<int16_t> &v, float target, bool mute, float &smooth)
{
    if (v.empty()) return;
    if (mute) { std::memset(v.data(), 0, v.size() * 2); smooth = target; return; }
    if (target != smooth) {
        const float start = smooth;
        const size_t n = v.size();
        for (size_t i = 0; i < n; ++i) {
            float g = start + (target - start) * ((float)(i + 1) / (float)n);
            int s = (int)(v[i] * g);
            v[i] = (int16_t)std::max(-32768, std::min(32767, s));
        }
        smooth = target;
    } else if (target != 1.0f) {
        for (auto &x : v) {
            int s = (int)(x * target);
            x = (int16_t)std::max(-32768, std::min(32767, s));
        }
    }
}

static void extra_worker(Extra *s)
{
    try {
        std::vector<int16_t> tmp;   // reused every iteration - no per-10ms allocation
        while (s->run.load()) {
            if (g_extra_paused.load()) {
                s->stream.drain_and_rebase();
                Sleep(10);
                continue;
            }
            if (s->stream.data_event) WaitForSingleObject(s->stream.data_event, 10);
            else Sleep(10);

            tmp.clear();
            s->stream.read(tmp);
            if (tmp.empty()) continue;

            const bool mute = s->mute.load();
            apply_gain(tmp, s->vol.load(), mute, s->smooth);
            s->level.store(mute ? 0 : calc_rms(tmp.data(), tmp.size()));

            if (s->buffering.load()) {
                std::lock_guard<std::mutex> lk(s->mu);
                s->buf.insert(s->buf.end(), tmp.begin(), tmp.end());
            }
        }
    } catch (const std::exception &e) {
        HrLog::Error(std::string("Extra audio source thread: uncaught exception (") + e.what() + ")");
    } catch (...) {
        HrLog::Error("Extra audio source thread: uncaught unknown exception");
    }
}

// Parses RIFF chunks (ffmpeg's WAVs carry a LIST chunk, so the fixed 44-byte header
// assumption wav_read() makes does not hold for them).
static bool wav_locate_data(FILE *f, uint32_t &rate, uint16_t &ch, long &off, uint32_t &bytes)
{
    char tag[4]; uint32_t sz = 0;
    if (fread(tag, 1, 4, f) != 4 || std::memcmp(tag, "RIFF", 4) != 0) return false;
    fseek(f, 4, SEEK_CUR);
    if (fread(tag, 1, 4, f) != 4 || std::memcmp(tag, "WAVE", 4) != 0) return false;
    bool have_fmt = false;
    while (fread(tag, 1, 4, f) == 4 && fread(&sz, 4, 1, f) == 1) {
        if (std::memcmp(tag, "fmt ", 4) == 0 && sz >= 16) {
            uint16_t fmt_tag = 0, bits = 0; uint32_t byte_rate = 0; uint16_t align = 0;
            if (fread(&fmt_tag, 2, 1, f) != 1 || fread(&ch, 2, 1, f) != 1 || fread(&rate, 4, 1, f) != 1 ||
                fread(&byte_rate, 4, 1, f) != 1 || fread(&align, 2, 1, f) != 1 || fread(&bits, 2, 1, f) != 1)
                return false;
            have_fmt = (bits == 16 && ch == 2);
            fseek(f, (long)sz - 16 + (long)(sz & 1), SEEK_CUR);
        } else if (std::memcmp(tag, "data", 4) == 0) {
            off = ftell(f);
            bytes = sz;
            return have_fmt;
        } else {
            fseek(f, (long)sz + (long)(sz & 1), SEEK_CUR);
        }
    }
    return false;
}

static Extra *find_extra_locked(int id)
{
    for (auto &p : g_extras) if (p->id == id) return p.get();
    return nullptr;
}

static void extra_stop_locked(Extra *s)
{
    s->run.store(false);
    if (s->th.joinable()) s->th.join();      // join BEFORE the COM objects are released
    s->stream.close();
    if (s->out_file) { fclose(s->out_file); s->out_file = nullptr; }
    if (s->rf)       { fclose(s->rf);       s->rf = nullptr; }
}

} // namespace hrx

// ---------------------------------------------------------------------------------
// Public C API
// ---------------------------------------------------------------------------------

/*  hr_audio_extra_add(kind, target, pid, volume, mute)
    kind 0: target = WASAPI endpoint id of the input device ("" = default input)
    kind 1: pid    = root process id whose sound is captured
    kind 2: target = path of a 44.1kHz/16-bit/stereo WAV
    Returns the channel handle (> 0) or a negative error:
      -1 unusable arguments, -2 device unavailable, -3 per-process capture unavailable
      (needs Windows 10 2004+ / the program has gone), -4 file unreadable. */
static int hr_audio_extra_add_impl(int kind, const wchar_t *target, unsigned long pid, float vol, int mute);

HR_EXPORT int hr_audio_extra_add(int kind, const wchar_t *target, unsigned long pid, float vol, int mute)
{
    // std::thread / make_unique / std::wstring can throw (std::system_error when the OS refuses a
    // thread, bad_alloc...). This function is called straight from the wx UI thread through an
    // extern "C" boundary, so an escaping exception ended in std::terminate = a crash right after
    // "select a program as a new audio source". Report it as "device unavailable" instead.
    try {
        return hr_audio_extra_add_impl(kind, target, pid, vol, mute);
    } catch (const std::exception &e) {
        HrLog::Error(std::string("Audio Mixer: adding a source failed (") + e.what() + ")");
    } catch (...) {
        HrLog::Error("Audio Mixer: adding a source failed (unknown exception)");
    }
    return -2;
}

static int hr_audio_extra_add_impl(int kind, const wchar_t *target, unsigned long pid, float vol, int mute)
{
    using namespace hrx;
    if (kind < 0 || kind > 2) return -1;
    auto s = std::make_unique<Extra>();
    s->kind = kind;
    s->target = target ? target : L"";
    s->pid = pid;
    s->vol.store(vol);
    s->smooth = vol;
    s->mute.store(mute != 0);

    if (kind == 0) {
        IMMDeviceEnumerator *e = make_enumerator();
        if (!e) return -2;
        IMMDevice *dev = find_input_by_id(e, s->target.c_str());
        if (!dev) dev = get_default_input(e);
        bool ok = dev && s->stream.open(false, dev);
        if (dev) dev->Release();
        e->Release();
        if (!ok) return -2;
    } else if (kind == 1) {
        if (!pid || !s->stream.open_process(pid)) return -3;
    } else {
        FILE *f = _wfopen(s->target.c_str(), L"rb");
        if (!f) return -4;
        uint16_t ch = 0;
        if (!wav_locate_data(f, s->file_rate, ch, s->data_off, s->data_bytes) || s->data_bytes == 0) {
            fclose(f);
            return -4;
        }
        s->rf = f;
        s->wav_path = s->target;
    }

    s->ok.store(true);
    if (kind != 2) {
        s->run.store(true);
        s->th = std::thread(extra_worker, s.get());
    }

    std::lock_guard<std::mutex> lk(g_extras_mu);
    s->id = g_extra_next_id++;
    const int id = s->id;
    g_extras.push_back(std::move(s));
    return id;
}

HR_EXPORT void hr_audio_extra_remove(int id)
{
    using namespace hrx;
    std::unique_ptr<Extra> dead;
    {
        std::lock_guard<std::mutex> lk(g_extras_mu);
        for (size_t i = 0; i < g_extras.size(); ++i) {
            if (g_extras[i]->id == id) { dead = std::move(g_extras[i]); g_extras.erase(g_extras.begin() + (ptrdiff_t)i); break; }
        }
    }
    if (dead) extra_stop_locked(dead.get());
}

HR_EXPORT void hr_audio_extra_set_volume(int id, float vol, int mute)
{
    using namespace hrx;
    std::lock_guard<std::mutex> lk(g_extras_mu);
    if (Extra *s = find_extra_locked(id)) { s->vol.store(vol); s->mute.store(mute != 0); }
}

HR_EXPORT int hr_audio_extra_level(int id)
{
    using namespace hrx;
    std::lock_guard<std::mutex> lk(g_extras_mu);
    Extra *s = find_extra_locked(id);
    if (!s) return 0;
    if (s->kind != 2) return s->level.load();
    // File channel: meter follows the file's playback position while recording.
    if (!g_extra_recording.load() || !s->rf || s->mute.load()) return 0;
    const double pos = rec_elapsed_sec();
    const long long frame = (long long)(pos * s->file_rate);
    const long long total = (long long)(s->data_bytes / 4);
    if (frame < 0 || frame + 1024 > total) return 0;
    int16_t win[2048];
    if (fseek(s->rf, s->data_off + (long)(frame * 4), SEEK_SET) != 0) return 0;
    const size_t got = fread(win, 2, 2048, s->rf);
    if (got < 2) return 0;
    const float g = s->vol.load();
    for (size_t i = 0; i < got; ++i) {
        int v = (int)(win[i] * g);
        win[i] = (int16_t)std::max(-32768, std::min(32767, v));
    }
    return calc_rms(win, got);
}

/*  1 = capturing, 0 = unknown handle */
HR_EXPORT int hr_audio_extra_status(int id)
{
    using namespace hrx;
    std::lock_guard<std::mutex> lk(g_extras_mu);
    Extra *s = find_extra_locked(id);
    return (s && s->ok.load()) ? 1 : 0;
}

/*  Called when a manual recording starts (right after hr_audio_reset_buffers). stem is
    the UTF-8 "<folder>/<name>" the video uses without extension. */
HR_EXPORT void hr_audio_extra_begin(const char *stem)
{
    using namespace hrx;
    std::lock_guard<std::mutex> lk(g_extras_mu);
    g_extra_stem = stem ? stem : "";
    QueryPerformanceFrequency(&g_extra_freq);
    QueryPerformanceCounter(&g_extra_t0);
    g_extra_paused_accum = 0.0;
    g_extra_in_pause = false;
    g_extra_paused.store(false);
    for (auto &p : g_extras) {
        Extra *s = p.get();
        if (s->kind == 2) continue;
        {
            std::lock_guard<std::mutex> bl(s->mu);
            s->buf.clear();
        }
        if (s->out_file) { fclose(s->out_file); s->out_file = nullptr; }
        s->out_bytes = 0;
        if (!g_extra_stem.empty())
            s->out_file = wav_stream_open((g_extra_stem + "_x" + std::to_string(s->id) + ".wav").c_str());
        s->buffering.store(s->out_file != nullptr);
    }
    g_extra_recording.store(true);
}

/*  Mirrors hr_audio_pause(): extras stop collecting (and the file timeline stops advancing). */
static void extra_on_pause(bool paused)
{
    using namespace hrx;
    g_extra_paused.store(paused);
    if (!g_extra_recording.load()) return;
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    if (paused && !g_extra_in_pause) {
        g_extra_in_pause = true;
        g_extra_pause_start = now;
    } else if (!paused && g_extra_in_pause) {
        g_extra_in_pause = false;
        if (g_extra_freq.QuadPart > 0)
            g_extra_paused_accum += (double)(now.QuadPart - g_extra_pause_start.QuadPart) / (double)g_extra_freq.QuadPart;
    }
}

/*  Drains the in-RAM chunks into the streaming WAVs (called from hr_audio_flush_buffered()). */
static void extra_flush()
{
    using namespace hrx;
    if (!g_extra_recording.load()) return;
    std::lock_guard<std::mutex> lk(g_extras_mu);
    for (auto &p : g_extras) {
        Extra *s = p.get();
        if (s->kind == 2 || !s->out_file) continue;
        std::vector<int16_t> chunk;
        {
            std::lock_guard<std::mutex> bl(s->mu);
            chunk.swap(s->buf);
        }
        if (!chunk.empty()) {
            wav_stream_append(s->out_file, chunk.data(), chunk.size());
            s->out_bytes += chunk.size() * 2;
        }
    }
}

/*  Finishes every channel's WAV and returns their UTF-8 paths separated by '|'
    (only channels that actually produced audio). Returns how many paths were written. */
HR_EXPORT int hr_audio_extra_collect(char *out, int cap)
{
    using namespace hrx;
    if (out && cap > 0) out[0] = '\0';
    extra_flush();

    std::lock_guard<std::mutex> lk(g_extras_mu);
    const double seconds = rec_elapsed_sec();
    g_extra_recording.store(false);
    g_extra_in_pause = false;

    std::string list;
    int count = 0;
    for (auto &p : g_extras) {
        Extra *s = p.get();
        const std::string path = g_extra_stem + "_x" + std::to_string(s->id) + ".wav";
        bool produced = false;

        if (s->kind != 2) {
            s->buffering.store(false);
            if (s->out_file) {
                std::vector<int16_t> tail;
                {
                    std::lock_guard<std::mutex> bl(s->mu);
                    tail.swap(s->buf);
                }
                if (!tail.empty()) {
                    wav_stream_append(s->out_file, tail.data(), tail.size());
                    s->out_bytes += tail.size() * 2;
                }
                wav_stream_close(s->out_file, 2, s->stream.rate ? s->stream.rate : 44100, s->out_bytes);
                s->out_file = nullptr;
                produced = s->out_bytes > 0;
                if (!produced) DeleteFileA(path.c_str());
            }
        } else if (s->rf && !g_extra_stem.empty() && seconds > 0.0) {
            // Render the file into the recording: from the start of the file, as long as
            // the recording lasted, with the fader's gain applied.
            uint64_t want = (uint64_t)(seconds * s->file_rate) * 4;
            if (want > s->data_bytes) want = s->data_bytes;
            want &= ~(uint64_t)3;
            FILE *o = wav_stream_open(path.c_str());
            if (o) {
                fseek(s->rf, s->data_off, SEEK_SET);
                const float g = s->mute.load() ? 0.0f : s->vol.load();
                std::vector<int16_t> chunk(32768);
                uint64_t done = 0;
                while (done < want) {
                    const size_t n = (size_t)std::min<uint64_t>((want - done) / 2, chunk.size());
                    const size_t got = fread(chunk.data(), 2, n, s->rf);
                    if (got == 0) break;
                    if (g != 1.0f) {
                        for (size_t i = 0; i < got; ++i) {
                            int v = (int)(chunk[i] * g);
                            chunk[i] = (int16_t)std::max(-32768, std::min(32767, v));
                        }
                    }
                    wav_stream_append(o, chunk.data(), got);
                    done += (uint64_t)got * 2;
                }
                wav_stream_close(o, 2, s->file_rate, done);
                produced = done > 0;
                if (!produced) DeleteFileA(path.c_str());
            }
        }

        if (produced) {
            if (!list.empty()) list += '|';
            list += path;
            ++count;
        }
    }
    if (out && cap > 0) {
        std::snprintf(out, (size_t)cap, "%s", list.c_str());
    }
    return count;
}

/*  App exit: stop every extra channel. */
HR_EXPORT void hr_audio_extra_shutdown()
{
    using namespace hrx;
    std::vector<std::unique_ptr<Extra>> all;
    {
        std::lock_guard<std::mutex> lk(g_extras_mu);
        all.swap(g_extras);
    }
    for (auto &p : all) extra_stop_locked(p.get());
}
