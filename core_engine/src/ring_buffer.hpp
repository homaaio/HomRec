#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <optional>
#include <d3d11.h>

namespace homrec {

// ---------------------------------------------------------------------------
//  FrameSlot — one cell in the ring buffer.
//  We store a COPY of the texture (not the DXGI pointer itself) so the
//  capture thread can immediately call ReleaseFrame and keep capturing.
// ---------------------------------------------------------------------------
struct FrameSlot {
    ID3D11Texture2D* texture = nullptr;
    UINT64           frame_id = 0;      // monotonic counter
    bool             filled   = false;

    void reset() noexcept {
        if (texture) { texture->Release(); texture = nullptr; }
        frame_id = 0;
        filled   = false;
    }
};

// ---------------------------------------------------------------------------
//  RingBuffer<N> — single-producer / single-consumer, wait-free.
//  The capture thread writes; the encoder thread reads.
//  If the buffer is full the oldest frame is dropped (overwrite mode).
// ---------------------------------------------------------------------------
template<std::size_t N>
class RingBuffer {
public:
    static_assert(N >= 4, "Buffer must have at least 4 slots");

    RingBuffer()  = default;
    ~RingBuffer() { clear(); }

    // -- Producer (capture thread) -------------------------------------------

    // Push a *already AddRef'd* texture.  Returns the frame_id assigned.
    UINT64 push(ID3D11Texture2D* tex, ID3D11Device* device) noexcept {
        UINT64 id = ++frame_counter_;
        std::size_t slot = id % N;

        // If there is an old frame in this slot, release it
        slots_[slot].reset();

        // Copy texture to staging so encoder can read it any time
        // (The original DXGI texture must NOT be held across ReleaseFrame)
        ID3D11DeviceContext* ctx = nullptr;
        device->GetImmediateContext(&ctx);

        D3D11_TEXTURE2D_DESC desc{};
        tex->GetDesc(&desc);
        desc.Usage          = D3D11_USAGE_STAGING;
        desc.BindFlags      = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        desc.MiscFlags      = 0;

        ID3D11Texture2D* staging = nullptr;
        HRESULT hr = device->CreateTexture2D(&desc, nullptr, &staging);
        if (SUCCEEDED(hr)) {
            ctx->CopyResource(staging, tex);
            slots_[slot].texture  = staging;   // caller owns the ref
            slots_[slot].frame_id = id;
            slots_[slot].filled   = true;
        }
        ctx->Release();

        // Update write head
        write_head_.store(id, std::memory_order_release);
        return id;
    }

    // -- Consumer (encoder thread) -------------------------------------------

    // Returns the next unread frame, or nullopt if nothing new.
    // The caller receives a *non-AddRef'd* pointer valid until next push()
    // on the SAME slot — safe because encoding is faster than capture gap.
    std::optional<UINT64> read_head() const noexcept {
        UINT64 w = write_head_.load(std::memory_order_acquire);
        UINT64 r = read_head_.load(std::memory_order_relaxed);
        if (w > r) return w;
        return std::nullopt;
    }

    FrameSlot& slot_at(UINT64 id) noexcept {
        return slots_[id % N];
    }

    void mark_read(UINT64 id) noexcept {
        read_head_.store(id, std::memory_order_release);
    }

    void clear() noexcept {
        for (auto& s : slots_) s.reset();
        write_head_.store(0, std::memory_order_relaxed);
        read_head_.store(0, std::memory_order_relaxed);
        frame_counter_.store(0, std::memory_order_relaxed);
    }

    UINT64 frames_captured()  const noexcept { return frame_counter_.load(); }
    UINT64 frames_dropped()   const noexcept {
        UINT64 w = write_head_.load();
        UINT64 r = read_head_.load();
        return (w > r + N) ? (w - r - N) : 0;
    }

private:
    std::array<FrameSlot, N> slots_{};
    std::atomic<UINT64>      write_head_{0};
    std::atomic<UINT64>      read_head_{0};
    std::atomic<UINT64>      frame_counter_{0};
};

// 3-second buffer at 120 FPS = 360 slots
using MainRingBuffer = RingBuffer<360>;

} // namespace homrec
