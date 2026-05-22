#pragma once
#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <d3d11.h>
#include "ring_buffer.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

namespace homrec {

enum class EncoderStatus {
    IDLE,
    RECORDING,
    STOPPING,
    FINISHED,
    ERROR,
};

struct EncoderConfig {
    std::string output_path;
    int         width       = 1920;
    int         height      = 1080;
    int         fps         = 60;
    int         bitrate_kbps = 8000;   // target bitrate (CBR hint)
    int         crf         = 18;      // quality (used in CRF mode)
    std::string codec       = "h264_nvenc";  // or libx264, h264_amf, h264_qsv
    std::string preset      = "p1";    // nvenc: p1-p7; x264: ultrafast..slow
    bool        use_crf     = false;   // if true, ignore bitrate_kbps
    int         audio_sample_rate = 44100;
    int         audio_channels    = 2;
};

using EncoderCallback = std::function<void(EncoderStatus, const std::string&)>;

// ---------------------------------------------------------------------------
class EncoderThread {
public:
    explicit EncoderThread(MainRingBuffer& ring_buf);
    ~EncoderThread();

    // Start encoding to file.  Requires CaptureThread to be running.
    bool start(const EncoderConfig& cfg,
               ID3D11Device*        device,
               ID3D11DeviceContext* context);

    // Stop encoding gracefully.  Blocks until the thread exits.
    void stop();

    void set_callback(EncoderCallback cb) { cb_ = std::move(cb); }

    EncoderStatus status() const noexcept { return status_.load(); }

    // Feed raw PCM audio (called from Python audio thread via bindings)
    void push_audio(const uint8_t* pcm, int byte_count);

    double encoding_fps()    const noexcept { return enc_fps_.load(); }
    UINT64 frames_encoded()  const noexcept { return frames_encoded_.load(); }

private:
    void thread_main(EncoderConfig cfg,
                     ID3D11Device* device,
                     ID3D11DeviceContext* context);

    // -- FFmpeg pipeline --------------------------------------------------
    bool open_output(const EncoderConfig& cfg);
    void close_output();
    bool send_video_frame(AVFrame* frame);
    void flush_encoder();

    // -- NV12 conversion --------------------------------------------------
    // Copies a D3D11 staging texture → CPU buffer → AVFrame (NV12 or YUV420)
    bool texture_to_avframe(ID3D11Texture2D*     tex,
                             ID3D11DeviceContext* ctx,
                             AVFrame*             out_frame);

    void report(EncoderStatus s, const std::string& msg = "");

    // -- Ring buffer ------------------------------------------------------
    MainRingBuffer& ring_;

    // -- FFmpeg objects ---------------------------------------------------
    AVFormatContext* fmt_ctx_   = nullptr;
    AVCodecContext*  codec_ctx_ = nullptr;
    AVStream*        video_stream_ = nullptr;
    SwsContext*      sws_ctx_   = nullptr;
    int64_t          pts_       = 0;

    // -- Thread -----------------------------------------------------------
    std::thread            thread_;
    std::atomic<bool>      stop_requested_{false};
    std::atomic<EncoderStatus> status_{EncoderStatus::IDLE};

    // -- Stats ------------------------------------------------------------
    std::atomic<double>  enc_fps_{0.0};
    std::atomic<UINT64>  frames_encoded_{0};

    EncoderCallback cb_;
};

} // namespace homrec
