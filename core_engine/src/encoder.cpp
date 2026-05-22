#include "encoder.hpp"
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace homrec {

// -- Constructor / Destructor -------------------------------------------------

EncoderThread::EncoderThread(MainRingBuffer& ring_buf)
    : ring_(ring_buf) {}

EncoderThread::~EncoderThread() {
    stop();
    close_output();
}

// -- start() ------------------------------------------------------------------

bool EncoderThread::start(const EncoderConfig& cfg,
                           ID3D11Device*        device,
                           ID3D11DeviceContext* context) {
    if (status_.load() == EncoderStatus::RECORDING) return false;

    stop_requested_.store(false);
    status_.store(EncoderStatus::RECORDING);
    pts_ = 0;

    thread_ = std::thread(&EncoderThread::thread_main, this,
                          cfg, device, context);
    return true;
}

// -- stop() -------------------------------------------------------------------

void EncoderThread::stop() {
    stop_requested_.store(true);
    if (thread_.joinable()) thread_.join();
}

// -- thread_main() ------------------------------------------------------------

void EncoderThread::thread_main(EncoderConfig cfg,
                                 ID3D11Device*        device,
                                 ID3D11DeviceContext* context) {
    using clock = std::chrono::steady_clock;

    if (!open_output(cfg)) {
        report(EncoderStatus::ERROR, "Failed to open output file");
        return;
    }

    report(EncoderStatus::RECORDING, "Encoder started: " + cfg.output_path);

    auto fps_t0     = clock::now();
    int  fps_frames = 0;

    // Allocate reusable AVFrame for encoding
    AVFrame* frame = av_frame_alloc();
    frame->format  = codec_ctx_->pix_fmt;
    frame->width   = codec_ctx_->width;
    frame->height  = codec_ctx_->height;
    av_frame_get_buffer(frame, 32);

    while (!stop_requested_.load()) {
        auto next_id_opt = ring_.read_head();
        if (!next_id_opt.has_value()) {
            // No new frame yet — yield briefly
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        UINT64 frame_id = *next_id_opt;
        auto&  slot     = ring_.slot_at(frame_id);

        if (!slot.filled || !slot.texture) {
            ring_.mark_read(frame_id);
            continue;
        }

        // -- Convert D3D11 texture → AVFrame ------------------------------
        av_frame_make_writable(frame);
        if (!texture_to_avframe(slot.texture, context, frame)) {
            ring_.mark_read(frame_id);
            continue;
        }

        ring_.mark_read(frame_id);

        // -- Set PTS -------------------------------------------------------
        frame->pts = pts_++;

        // -- Encode -------------------------------------------------------
        send_video_frame(frame);

        // -- FPS -----------------------------------------------------------
        ++fps_frames;
        ++frames_encoded_;
        auto now     = clock::now();
        double elapsed = std::chrono::duration<double>(now - fps_t0).count();
        if (elapsed >= 1.0) {
            enc_fps_.store(fps_frames / elapsed);
            fps_frames = 0;
            fps_t0     = now;
        }
    }

    av_frame_free(&frame);

    // -- Flush encoder -----------------------------------------------------
    flush_encoder();
    close_output();

    status_.store(EncoderStatus::FINISHED);
    report(EncoderStatus::FINISHED, "Encoding finished: " + cfg.output_path);
}

// -- open_output() ------------------------------------------------------------

bool EncoderThread::open_output(const EncoderConfig& cfg) {
    // Format context
    int ret = avformat_alloc_output_context2(&fmt_ctx_, nullptr, nullptr,
                                              cfg.output_path.c_str());
    if (ret < 0 || !fmt_ctx_) return false;

    // Codec
    const AVCodec* codec = avcodec_find_encoder_by_name(cfg.codec.c_str());
    if (!codec) {
        // Fallback to software x264
        codec = avcodec_find_encoder_by_name("libx264");
        if (!codec) return false;
    }

    video_stream_ = avformat_new_stream(fmt_ctx_, nullptr);
    if (!video_stream_) return false;

    codec_ctx_ = avcodec_alloc_context3(codec);
    if (!codec_ctx_) return false;

    codec_ctx_->width     = cfg.width;
    codec_ctx_->height    = cfg.height;
    codec_ctx_->time_base = { 1, cfg.fps };
    codec_ctx_->framerate = { cfg.fps, 1 };
    codec_ctx_->pix_fmt   = AV_PIX_FMT_YUV420P;  // fallback; nvenc uses NV12 internally
    codec_ctx_->gop_size  = cfg.fps * 2;
    codec_ctx_->max_b_frames = 0;   // zero for real-time

    if (fmt_ctx_->oformat->flags & AVFMT_GLOBALHEADER)
        codec_ctx_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    // Quality / bitrate
    if (cfg.use_crf) {
        av_opt_set_int(codec_ctx_->priv_data, "crf",    cfg.crf, 0);
    } else {
        codec_ctx_->bit_rate = (int64_t)cfg.bitrate_kbps * 1000;
    }
    av_opt_set(codec_ctx_->priv_data, "preset", cfg.preset.c_str(), 0);

    // NVENC-specific: zero-latency tuning
    if (cfg.codec.find("nvenc") != std::string::npos) {
        av_opt_set(codec_ctx_->priv_data, "tune",   "ull",  0);
        av_opt_set(codec_ctx_->priv_data, "rc",     "vbr",  0);
        av_opt_set(codec_ctx_->priv_data, "zerolatency", "1", 0);
    }
    if (cfg.codec == "libx264" || cfg.codec == "libx265") {
        av_opt_set(codec_ctx_->priv_data, "tune", "zerolatency", 0);
    }

    if (avcodec_open2(codec_ctx_, codec, nullptr) < 0) return false;

    avcodec_parameters_from_context(video_stream_->codecpar, codec_ctx_);
    video_stream_->time_base = codec_ctx_->time_base;

    // SwsContext: BGRA → YUV420P
    sws_ctx_ = sws_getContext(
        cfg.width, cfg.height, AV_PIX_FMT_BGRA,
        cfg.width, cfg.height, AV_PIX_FMT_YUV420P,
        SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws_ctx_) return false;

    // Open file I/O
    if (!(fmt_ctx_->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&fmt_ctx_->pb, cfg.output_path.c_str(), AVIO_FLAG_WRITE) < 0)
            return false;
    }

    // Write container header
    if (avformat_write_header(fmt_ctx_, nullptr) < 0) return false;

    return true;
}

void EncoderThread::close_output() {
    if (fmt_ctx_) {
        av_write_trailer(fmt_ctx_);
        if (!(fmt_ctx_->oformat->flags & AVFMT_NOFILE))
            avio_closep(&fmt_ctx_->pb);
        avformat_free_context(fmt_ctx_);
        fmt_ctx_ = nullptr;
    }
    if (codec_ctx_) {
        avcodec_free_context(&codec_ctx_);
        codec_ctx_ = nullptr;
    }
    if (sws_ctx_) {
        sws_freeContext(sws_ctx_);
        sws_ctx_ = nullptr;
    }
    video_stream_ = nullptr;
    pts_ = 0;
}

// -- texture_to_avframe() -----------------------------------------------------

bool EncoderThread::texture_to_avframe(ID3D11Texture2D*     tex,
                                        ID3D11DeviceContext* ctx,
                                        AVFrame*             out_frame) {
    D3D11_TEXTURE2D_DESC desc{};
    tex->GetDesc(&desc);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    HRESULT hr = ctx->Map(tex, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) return false;

    // mapped.pData is BGRA rows, stride = mapped.RowPitch
    const uint8_t* src_data[1] = {
        reinterpret_cast<const uint8_t*>(mapped.pData)
    };
    int src_linesize[1] = { static_cast<int>(mapped.RowPitch) };

    // Convert BGRA → YUV420P via swscale
    sws_scale(sws_ctx_,
              src_data, src_linesize,
              0, static_cast<int>(desc.Height),
              out_frame->data, out_frame->linesize);

    ctx->Unmap(tex, 0);
    return true;
}

// -- send_video_frame() -------------------------------------------------------

bool EncoderThread::send_video_frame(AVFrame* frame) {
    if (avcodec_send_frame(codec_ctx_, frame) < 0) return false;

    AVPacket* pkt = av_packet_alloc();
    while (avcodec_receive_packet(codec_ctx_, pkt) == 0) {
        av_packet_rescale_ts(pkt, codec_ctx_->time_base,
                              video_stream_->time_base);
        pkt->stream_index = video_stream_->index;
        av_interleaved_write_frame(fmt_ctx_, pkt);
    }
    av_packet_free(&pkt);
    return true;
}

// -- flush_encoder() ----------------------------------------------------------

void EncoderThread::flush_encoder() {
    if (!codec_ctx_) return;
    avcodec_send_frame(codec_ctx_, nullptr);  // flush signal

    AVPacket* pkt = av_packet_alloc();
    while (avcodec_receive_packet(codec_ctx_, pkt) == 0) {
        av_packet_rescale_ts(pkt, codec_ctx_->time_base,
                              video_stream_->time_base);
        pkt->stream_index = video_stream_->index;
        av_interleaved_write_frame(fmt_ctx_, pkt);
    }
    av_packet_free(&pkt);
}

void EncoderThread::report(EncoderStatus s, const std::string& msg) {
    status_.store(s);
    if (cb_) cb_(s, msg);
}

} // namespace homrec
