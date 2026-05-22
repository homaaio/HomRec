#pragma once
/*
 * frame_pool.hpp — HomRec 2.0  (C++17)
 * Lock-free staging-texture pool.
 *
 * Problem: every captured frame previously called CreateTexture2D() to
 * create a CPU-readable staging copy.  That's an expensive driver round-
 * trip on every frame (especially at 120 FPS).
 *
 * Solution: keep a small pool of pre-allocated staging textures and recycle
 * them instead of destroying and re-creating on every frame.
 *
 * Thread model
 * ------------
 *   Capture thread  → calls acquire() / release()
 *   Encoder thread  → calls acquire() / release()
 *
 * Both paths are lock-free (CAS on an atomic bitmask).  No mutex, no
 * condition variable, no heap allocation after initialisation.
 */

#include <array>
#include <atomic>
#include <cstdint>
#include <d3d11.h>

namespace homrec {

// -- FramePool ----------------------------------------------------------------

template<std::size_t N = 16>
class FramePool {
    static_assert(N <= 64,  "Pool size must be ≤ 64 (bitmask is uint64_t)");
    static_assert(N >= 4,   "Pool size must be ≥ 4");

public:
    // Must be called once from the capture thread before any acquire().
    // desc.Usage, BindFlags and CPUAccessFlags are overwritten to match
    // a D3D11 staging texture.
    bool init(ID3D11Device* device, D3D11_TEXTURE2D_DESC desc) {
        desc.Usage          = D3D11_USAGE_STAGING;
        desc.BindFlags      = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        desc.MiscFlags      = 0;

        for (std::size_t i = 0; i < N; ++i) {
            HRESULT hr = device->CreateTexture2D(&desc, nullptr, &slots_[i]);
            if (FAILED(hr)) { destroy(); return false; }
        }
        free_mask_.store((UINT64(1) << N) - 1, std::memory_order_release);
        return true;
    }

    void destroy() noexcept {
        for (auto& t : slots_) {
            if (t) { t->Release(); t = nullptr; }
        }
        free_mask_.store(0, std::memory_order_release);
    }

    // -- acquire() ---------------------------------------------------------
    // Returns a slot index in [0, N) on success, or -1 when the pool is
    // empty.  Non-blocking — callers must handle the -1 case gracefully
    // (e.g. drop the frame).
    int acquire() noexcept {
        UINT64 mask = free_mask_.load(std::memory_order_relaxed);
        while (mask) {
            int bit = __builtin_ctzll(mask);         // lowest set bit
            UINT64 desired = mask & ~(UINT64(1) << bit);
            if (free_mask_.compare_exchange_weak(
                    mask, desired,
                    std::memory_order_acquire,
                    std::memory_order_relaxed)) {
                return bit;
            }
            // mask was refreshed by the CAS failure — retry
        }
        return -1;  // pool exhausted
    }

    // release() must be called exactly once for every successful acquire().
    void release(int slot) noexcept {
        free_mask_.fetch_or(UINT64(1) << slot, std::memory_order_release);
    }

    ID3D11Texture2D* texture(int slot) noexcept { return slots_[slot]; }
    std::size_t      size()   const noexcept    { return N; }

private:
    std::array<ID3D11Texture2D*, N> slots_{};
    std::atomic<UINT64>             free_mask_{0};
};

// Default pool used by CaptureThread / EncoderThread
using DefaultFramePool = FramePool<16>;

} // namespace homrec
