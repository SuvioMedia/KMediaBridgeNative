/* SPDX-License-Identifier: LGPL-2.1-or-later */

#ifndef KMEDIA_BRIDGE_HDR_MATH_H
#define KMEDIA_BRIDGE_HDR_MATH_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum KmbHdrTransfer {
    KMB_HDR_TRANSFER_PQ = 1,
    KMB_HDR_TRANSFER_HLG = 2
} KmbHdrTransfer;

typedef struct KmbHdrColorTransform KmbHdrColorTransform;

KmbHdrColorTransform *kmb_hdr_color_transform_create(
    KmbHdrTransfer transfer,
    double source_peak_nits,
    double target_white_nits
);

void kmb_hdr_color_transform_free(KmbHdrColorTransform **transform);

/*
 * Converts one nonlinear BT.2020 RGB sample into nonlinear BT.709 SDR.
 * Inputs and outputs use normalized full-range floating-point code values.
 */
int kmb_hdr_color_transform_pixel(
    const KmbHdrColorTransform *transform,
    float source_red,
    float source_green,
    float source_blue,
    float *output_red,
    float *output_green,
    float *output_blue
);

/* In-place conversion of an AV_PIX_FMT_GBRPF32 frame. */
int kmb_hdr_color_transform_gbrpf32(
    const KmbHdrColorTransform *transform,
    float *green,
    ptrdiff_t green_stride_bytes,
    float *blue,
    ptrdiff_t blue_stride_bytes,
    float *red,
    ptrdiff_t red_stride_bytes,
    int width,
    int height
);

/* Reference hooks used by native conformance tests. */
double kmb_hdr_pq_eotf(double encoded);
double kmb_hdr_pq_oetf(double luminance_nits);
double kmb_hdr_hlg_inverse_oetf(double encoded);
double kmb_hdr_tone_map_bt2390(
    double luminance_nits,
    double source_peak_nits,
    double target_peak_nits
);

#ifdef __cplusplus
}
#endif

#endif
