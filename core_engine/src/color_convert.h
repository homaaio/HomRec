/*
 * color_convert.h — HomRec 2.0
 * Fast BGRA ↔ YUV color-space conversion and frame scaling utilities.
 * Callable from C++ (included by encoder.cpp) and from C.
 */
#pragma once
#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* BGRA → NV12 (semi-planar, interleaved UV) */
void homrec_bgra_to_nv12(
    const uint8_t* bgra,  int bgra_stride,
    uint8_t*       y_plane,  int y_stride,
    uint8_t*       uv_plane, int uv_stride,
    int width, int height);

/* BGRA → planar YUV420 */
void homrec_bgra_to_yuv420(
    const uint8_t* bgra,  int bgra_stride,
    uint8_t*       y_plane, int y_stride,
    uint8_t*       u_plane, int u_stride,
    uint8_t*       v_plane, int v_stride,
    int width, int height);

/* 2× bilinear downscale, BGRA in/out */
void homrec_downscale_bilinear_2x(
    const uint8_t* src,  int src_stride,
    uint8_t*       dst,  int dst_stride,
    int src_width, int src_height);

#ifdef __cplusplus
}
#endif
