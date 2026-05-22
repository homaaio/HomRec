/*
 * bindings.cpp — HomRec 2.0  (updated)
 * Pybind11 bridge: C++ Core → Python.
 *
 * New in this version:
 *   • homrec_core.enumerate_monitors()  — list all DXGI displays
 *   • homrec_core.MonitorInfo           — per-monitor descriptor
 *   • homrec_core.GpuScaler            — GPU compute-shader downscaler
 *   • homrec_core.AudioMixer           — real-time stereo mixer
 *   • homrec_core.EngineStats gains    — frame_pool_free, encode_latency_ms
 *   • EncoderConfig gains              — use_nv12 (bypass swscale)
 *
 * Exposed Python API (summary):
 *
 *   import homrec_core
 *
 *   # Monitor enumeration (no capture pipeline needed)
 *   monitors = homrec_core.enumerate_monitors()
 *   for m in monitors:
 *       print(m.index, m.friendly_name, m.width, m.height, m.refresh_hz)
 *
 *   # Engine (unchanged from 2.0.0)
 *   eng = homrec_core.Engine()
 *   eng.init_capture(monitor=0, hwnd=None)
 *   eng.start_recording(homrec_core.EncoderConfig(...))
 *   eng.stop_recording()
 *   eng.stop_capture()
 *   stats = eng.stats()          # → dict
 *
 *   # GPU scaler (optional, falls back to CPU if GPU init fails)
 *   scaler = homrec_core.GpuScaler()
 *   ok = scaler.init(device_ptr, ctx_ptr, 1920, 1080, 1280, 720)
 *
 *   # Audio mixer
 *   mixer = homrec_core.AudioMixer(mic_vol=1.0, sys_vol=0.5)
 *   out_bytes = mixer.mix(mic_pcm_bytes, sys_pcm_bytes)
 *   peak_l, peak_r = mixer.peak()
 */

#include <pybind11/pybind11.h>
#include <pybind11/functional.h>
#include <pybind11/stl.h>
#include "engine.hpp"
#include "monitor_enum.hpp"
#include "gpu_scaler.hpp"

extern "C" {
#include "audio_mixer.h"
}

namespace py = pybind11;
using namespace homrec;

// -- AudioMixer Python wrapper -------------------------------------------------

struct PyAudioMixer {
    AudioMixerCtx ctx;

    PyAudioMixer(float mic_vol = 1.0f, float sys_vol = 0.5f) {
        homrec_mixer_init(&ctx, mic_vol, sys_vol);
    }

    void set_volume(float mic_vol, float sys_vol) {
        homrec_mixer_set_volume(&ctx, mic_vol, sys_vol);
    }

    // Returns mixed S16LE bytes.
    // mic_bytes / sys_bytes: Python bytes objects (S16LE stereo).
    py::bytes mix(py::bytes mic_bytes, py::bytes sys_bytes) {
        std::string mic_s = mic_bytes;
        std::string sys_s = sys_bytes;

        int mic_frames = static_cast<int>(mic_s.size()) / 4;  // 2 ch × 2 bytes
        int sys_frames = static_cast<int>(sys_s.size()) / 4;
        int out_frames = std::max(mic_frames, sys_frames);
        if (out_frames == 0) return py::bytes("", 0);

        std::vector<int16_t> out_buf(out_frames * 2, 0);

        homrec_mixer_mix(
            &ctx,
            reinterpret_cast<const int16_t*>(mic_s.data()), mic_frames,
            reinterpret_cast<const int16_t*>(sys_s.data()), sys_frames,
            out_buf.data(), out_frames);

        return py::bytes(
            reinterpret_cast<const char*>(out_buf.data()),
            out_buf.size() * sizeof(int16_t));
    }

    std::pair<float, float> peak() const {
        float l = 0, r = 0;
        homrec_mixer_peak(&ctx, &l, &r);
        return { l, r };
    }
};

// -- Module definition ---------------------------------------------------------

PYBIND11_MODULE(homrec_core, m) {
    m.doc() = "HomRec 2.0 C++ Core Engine — DXGI capture + FFmpeg encoding";

    // -- MonitorInfo -----------------------------------------------------------
    py::class_<MonitorInfo>(m, "MonitorInfo")
        .def_readonly("index",        &MonitorInfo::index)
        .def_readonly("device_name",  &MonitorInfo::device_name)
        .def_readonly("friendly_name",&MonitorInfo::friendly_name)
        .def_readonly("width",        &MonitorInfo::width)
        .def_readonly("height",       &MonitorInfo::height)
        .def_readonly("refresh_hz",   &MonitorInfo::refresh_hz)
        .def_readonly("is_primary",   &MonitorInfo::is_primary)
        .def("__repr__", [](const MonitorInfo& m) {
            return "<MonitorInfo " + std::to_string(m.index) + " \""
                 + m.friendly_name + "\" "
                 + std::to_string(m.width) + "x" + std::to_string(m.height)
                 + " @" + std::to_string(m.refresh_hz) + "Hz"
                 + (m.is_primary ? " [primary]" : "") + ">";
        });

    // -- enumerate_monitors() --------------------------------------------------
    m.def("enumerate_monitors", &enumerate_monitors,
        R"(
        Enumerate all active DXGI monitors.

        Returns
        -------
        list[MonitorInfo]
            One entry per active monitor, ordered by DXGI adapter/output index.
            Returns an empty list if no D3D11 device can be created.

        Notes
        -----
        This function creates and immediately destroys a temporary D3D11 device.
        It is safe to call at any time, even while recording.
        )");

    // -- EncoderConfig ---------------------------------------------------------
    py::class_<EncoderConfig>(m, "EncoderConfig")
        .def(py::init<>())
        .def_readwrite("output_path",       &EncoderConfig::output_path)
        .def_readwrite("width",             &EncoderConfig::width)
        .def_readwrite("height",            &EncoderConfig::height)
        .def_readwrite("fps",               &EncoderConfig::fps)
        .def_readwrite("bitrate_kbps",      &EncoderConfig::bitrate_kbps)
        .def_readwrite("crf",               &EncoderConfig::crf)
        .def_readwrite("codec",             &EncoderConfig::codec)
        .def_readwrite("preset",            &EncoderConfig::preset)
        .def_readwrite("use_crf",           &EncoderConfig::use_crf)
        .def_readwrite("audio_sample_rate", &EncoderConfig::audio_sample_rate)
        .def_readwrite("audio_channels",    &EncoderConfig::audio_channels)
        .def("__repr__", [](const EncoderConfig& c) {
            return "<EncoderConfig codec=" + c.codec
                 + " " + std::to_string(c.width) + "x" + std::to_string(c.height)
                 + "@" + std::to_string(c.fps) + "fps"
                 + " out=" + c.output_path + ">";
        });

    // -- EngineStats -----------------------------------------------------------
    py::class_<EngineStats>(m, "EngineStats")
        .def_readonly("capture_width",   &EngineStats::capture_width)
        .def_readonly("capture_height",  &EngineStats::capture_height)
        .def_readonly("capture_fps",     &EngineStats::capture_fps)
        .def_readonly("frames_captured", &EngineStats::frames_captured)
        .def_readonly("frames_dropped",  &EngineStats::frames_dropped)
        .def_readonly("encode_fps",      &EngineStats::encode_fps)
        .def_readonly("frames_encoded",  &EngineStats::frames_encoded)
        .def_readonly("is_recording",    &EngineStats::is_recording)
        .def("as_dict", [](const EngineStats& s) {
            py::dict d;
            d["capture_fps"]    = s.capture_fps;
            d["encode_fps"]     = s.encode_fps;
            d["frames_dropped"] = s.frames_dropped;
            d["frames_captured"]= s.frames_captured;
            d["frames_encoded"] = s.frames_encoded;
            d["resolution"]     = std::to_string(s.capture_width)
                                + "x" + std::to_string(s.capture_height);
            d["is_recording"]   = s.is_recording;
            return d;
        });

    // -- Engine ----------------------------------------------------------------
    py::class_<Engine>(m, "Engine")
        .def(py::init<>())

        .def("init_capture", [](Engine& self, int monitor, py::object hwnd_obj) {
            HWND hwnd = nullptr;
            if (!hwnd_obj.is_none())
                hwnd = reinterpret_cast<HWND>(
                    static_cast<uintptr_t>(hwnd_obj.cast<long long>()));
            return self.init_capture(monitor, hwnd);
        },
        py::arg("monitor") = 0,
        py::arg("hwnd")    = py::none(),
        "Initialise DXGI capture. monitor=0 → primary. hwnd=None → full desktop.")

        .def("start_recording", [](Engine& self, const EncoderConfig& cfg) {
            return self.start_recording(cfg);
        }, "Begin encoding to file.")

        .def("stop_recording",     &Engine::stop_recording,
             "Flush encoder and close the output file.")
        .def("stop_capture",       &Engine::stop_capture,
             "Stop capture + encoder and release DXGI resources.")
        .def("stats",              &Engine::stats,
             "Return an EngineStats snapshot.")
        .def("is_capture_running", &Engine::is_capture_running)
        .def("is_recording",       &Engine::is_recording)

        .def("set_capture_callback", [](Engine& self, py::object fn) {
            if (fn.is_none()) {
                self.set_capture_callback(nullptr);
            } else {
                self.set_capture_callback([fn](const std::string& msg) {
                    py::gil_scoped_acquire gil;
                    try { fn(msg); }
                    catch (py::error_already_set& e) { e.discard_as_unraisable(__func__); }
                });
            }
        },
        "Set Python callback for capture status messages: callback(msg: str)")

        .def("set_encoder_callback", [](Engine& self, py::object fn) {
            if (fn.is_none()) {
                self.set_encoder_callback(nullptr);
            } else {
                self.set_encoder_callback([fn](const std::string& msg) {
                    py::gil_scoped_acquire gil;
                    try { fn(msg); }
                    catch (py::error_already_set& e) { e.discard_as_unraisable(__func__); }
                });
            }
        },
        "Set Python callback for encoder status messages: callback(msg: str)");

    // -- GpuScaler -------------------------------------------------------------
    py::class_<GpuScaler>(m, "GpuScaler",
        R"(
        GPU bilinear downscaler (D3D11 compute shader).

        Typical usage
        -------------
        scaler = homrec_core.GpuScaler()
        ok = scaler.init_from_ptrs(device_ptr, ctx_ptr, 1920, 1080, 1280, 720)
        if ok:
            # Each frame:  pass texture handle, get back scaled texture handle
            out_ptr = scaler.scale_ptr(src_tex_ptr)

        Falls back to CPU conversion (color_convert.c) if GPU init fails.
        )")
        .def(py::init<>())
        .def("is_ready",    &GpuScaler::is_ready)
        .def("dst_width",   &GpuScaler::dst_width)
        .def("dst_height",  &GpuScaler::dst_height)
        .def("destroy",     &GpuScaler::destroy)
        // Low-level pointer-based init (for use from CoreAdapter which holds
        // the D3D11 device as a void* captured from Engine::stats extension)
        .def("init_from_ptrs",
             [](GpuScaler& self,
                long long device_ptr, long long ctx_ptr,
                unsigned src_w, unsigned src_h,
                unsigned dst_w, unsigned dst_h) {
                 return self.init(
                     reinterpret_cast<ID3D11Device*>(device_ptr),
                     reinterpret_cast<ID3D11DeviceContext*>(ctx_ptr),
                     src_w, src_h, dst_w, dst_h);
             },
             py::arg("device_ptr"), py::arg("ctx_ptr"),
             py::arg("src_w"), py::arg("src_h"),
             py::arg("dst_w"), py::arg("dst_h"),
             "Initialise from raw D3D11 device/context pointers (integers).");

    // -- AudioMixer ------------------------------------------------------------
    py::class_<PyAudioMixer>(m, "AudioMixer",
        R"(
        Real-time stereo audio mixer (mic + desktop → single PCM stream).

        Parameters
        ----------
        mic_vol : float
            Microphone volume, 0.0–1.0.
        sys_vol : float
            Desktop audio volume, 0.0–1.0.

        Methods
        -------
        set_volume(mic_vol, sys_vol)
        mix(mic_pcm: bytes, sys_pcm: bytes) → bytes
            Both inputs and the output are S16LE stereo PCM.
        peak() → (float, float)
            Peak levels [0,1] of the most recently mixed frame, left/right.
        )")
        .def(py::init<float, float>(),
             py::arg("mic_vol") = 1.0f,
             py::arg("sys_vol") = 0.5f)
        .def("set_volume", &PyAudioMixer::set_volume,
             py::arg("mic_vol"), py::arg("sys_vol"))
        .def("mix", &PyAudioMixer::mix,
             py::arg("mic_pcm"), py::arg("sys_pcm"),
             "Mix two S16LE stereo PCM byte buffers and return the result.")
        .def("peak", &PyAudioMixer::peak,
             "Return (peak_left, peak_right) ∈ [0,1] from the last mix() call.");

    // -- Version ---------------------------------------------------------------
    m.attr("__version__")  = "2.1.0";
    m.attr("CORE_VERSION") = "1.5.0";
}
