/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include "kmedia_bridge_hdr_math.h"

#include <assert.h>
#include <math.h>

static int close_to(double actual, double expected, double tolerance) {
    return fabs(actual - expected) <= tolerance;
}

static void test_reference_transfer_points(void) {
    assert(close_to(kmb_hdr_pq_eotf(0.0), 0.0, 1.0e-9));
    assert(close_to(kmb_hdr_pq_eotf(1.0), 10000.0, 1.0e-4));
    assert(close_to(kmb_hdr_pq_eotf(kmb_hdr_pq_oetf(100.0)), 100.0, 1.0e-5));
    assert(close_to(kmb_hdr_hlg_inverse_oetf(0.5), 1.0 / 12.0, 1.0e-9));
}

static void test_bt2390_is_monotonic_and_bounded(void) {
    double previous = 0.0;
    int sample = 0;
    for (sample = 0; sample <= 400; ++sample) {
        const double mapped = kmb_hdr_tone_map_bt2390(sample * 10.0, 4000.0, 100.0);
        assert(mapped + 1.0e-8 >= previous);
        assert(mapped >= 0.0 && mapped <= 100.0 + 1.0e-8);
        previous = mapped;
    }
}

static void test_pq_and_hlg_emit_bounded_bt709(void) {
    KmbHdrColorTransform *pq = kmb_hdr_color_transform_create(KMB_HDR_TRANSFER_PQ, 1000.0, 100.0);
    KmbHdrColorTransform *hlg = kmb_hdr_color_transform_create(KMB_HDR_TRANSFER_HLG, 1000.0, 100.0);
    float red = 0.0f;
    float green = 0.0f;
    float blue = 0.0f;
    assert(pq != NULL && hlg != NULL);
    assert(kmb_hdr_color_transform_pixel(pq, 1.0f, 1.0f, 1.0f, &red, &green, &blue));
    assert(red >= 0.99f && red <= 1.0f);
    assert(green >= 0.99f && green <= 1.0f);
    assert(blue >= 0.99f && blue <= 1.0f);
    assert(kmb_hdr_color_transform_pixel(hlg, 0.75f, 0.2f, 0.1f, &red, &green, &blue));
    assert(red >= 0.0f && red <= 1.0f);
    assert(green >= 0.0f && green <= 1.0f);
    assert(blue >= 0.0f && blue <= 1.0f);
    assert(red > green && green >= blue);
    kmb_hdr_color_transform_free(&pq);
    kmb_hdr_color_transform_free(&hlg);
    assert(pq == NULL && hlg == NULL);
}

int main(void) {
    test_reference_transfer_points();
    test_bt2390_is_monotonic_and_bounded();
    test_pq_and_hlg_emit_bounded_bt709();
    return 0;
}
