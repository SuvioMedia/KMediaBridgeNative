/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include "kmedia_bridge_hdr_math.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>

#define KMB_TRANSFER_LUT_SIZE 4096
#define KMB_GAMUT_LUT_EDGE 33
#define KMB_GAMUT_LUT_CHANNELS 3
#define KMB_GAMUT_SEARCH_STEPS 16

#define KMB_PQ_M1 (2610.0 / 16384.0)
#define KMB_PQ_M2 (2523.0 / 32.0)
#define KMB_PQ_C1 (3424.0 / 4096.0)
#define KMB_PQ_C2 (2413.0 / 128.0)
#define KMB_PQ_C3 (2392.0 / 128.0)
#define KMB_PQ_MAX_NITS 10000.0

#define KMB_HLG_A 0.17883277
#define KMB_HLG_B (1.0 - 4.0 * KMB_HLG_A)

typedef struct KmbRgb {
    double red;
    double green;
    double blue;
} KmbRgb;

typedef struct KmbIctcp {
    double intensity;
    double tritan;
    double protan;
} KmbIctcp;

struct KmbHdrColorTransform {
    KmbHdrTransfer transfer;
    double source_peak_nits;
    double target_white_nits;
    float transfer_lut[KMB_TRANSFER_LUT_SIZE];
    float tone_lut[KMB_TRANSFER_LUT_SIZE];
    float hlg_ootf_scale_lut[KMB_TRANSFER_LUT_SIZE];
    float bt709_oetf_lut[KMB_TRANSFER_LUT_SIZE];
    float gamut_lut[
        KMB_GAMUT_LUT_EDGE * KMB_GAMUT_LUT_EDGE *
        KMB_GAMUT_LUT_EDGE * KMB_GAMUT_LUT_CHANNELS
    ];
};

static double kmb_clamp(double value, double minimum, double maximum) {
    if (value < minimum) return minimum;
    if (value > maximum) return maximum;
    return value;
}

static double kmb_hlg_c(void) {
    return 0.5 - KMB_HLG_A * log(4.0 * KMB_HLG_A);
}

double kmb_hdr_pq_eotf(double encoded) {
    const double signal = kmb_clamp(encoded, 0.0, 1.0);
    const double powered = pow(signal, 1.0 / KMB_PQ_M2);
    const double numerator = fmax(powered - KMB_PQ_C1, 0.0);
    const double denominator = KMB_PQ_C2 - KMB_PQ_C3 * powered;
    if (denominator <= 0.0) return KMB_PQ_MAX_NITS;
    return pow(numerator / denominator, 1.0 / KMB_PQ_M1) * KMB_PQ_MAX_NITS;
}

double kmb_hdr_pq_oetf(double luminance_nits) {
    const double normalized = pow(
        kmb_clamp(luminance_nits, 0.0, KMB_PQ_MAX_NITS) / KMB_PQ_MAX_NITS,
        KMB_PQ_M1
    );
    return pow(
        (KMB_PQ_C1 + KMB_PQ_C2 * normalized) /
            (1.0 + KMB_PQ_C3 * normalized),
        KMB_PQ_M2
    );
}

double kmb_hdr_hlg_inverse_oetf(double encoded) {
    const double signal = fmax(encoded, 0.0);
    if (signal <= 0.5) return signal * signal / 3.0;
    return (exp((signal - kmb_hlg_c()) / KMB_HLG_A) + KMB_HLG_B) / 12.0;
}

static double kmb_hermite(
    double t,
    double start,
    double start_slope,
    double end,
    double end_slope
) {
    const double t2 = t * t;
    const double t3 = t2 * t;
    return (2.0 * t3 - 3.0 * t2 + 1.0) * start +
        (t3 - 2.0 * t2 + t) * start_slope +
        (-2.0 * t3 + 3.0 * t2) * end +
        (t3 - t2) * end_slope;
}

double kmb_hdr_tone_map_bt2390(
    double luminance_nits,
    double source_peak_nits,
    double target_peak_nits
) {
    double source_peak_code = 0.0;
    double target_peak_code = 0.0;
    double normalized_target = 0.0;
    double knee = 0.0;
    double normalized_input = 0.0;
    double normalized_output = 0.0;
    if (!isfinite(source_peak_nits) || !isfinite(target_peak_nits) ||
        source_peak_nits <= 0.0 || target_peak_nits <= 0.0) {
        return 0.0;
    }
    if (target_peak_nits >= source_peak_nits) {
        return kmb_clamp(luminance_nits, 0.0, source_peak_nits);
    }

    source_peak_code = kmb_hdr_pq_oetf(source_peak_nits);
    target_peak_code = kmb_hdr_pq_oetf(target_peak_nits);
    normalized_target = kmb_clamp(target_peak_code / source_peak_code, 0.0, 1.0);
    knee = kmb_clamp(1.5 * normalized_target - 0.5, 0.0, 1.0);
    normalized_input = kmb_clamp(
        kmb_hdr_pq_oetf(luminance_nits) / source_peak_code,
        0.0,
        1.0
    );
    if (normalized_input <= knee || knee >= 1.0) {
        normalized_output = normalized_input;
    } else {
        const double t = kmb_clamp((normalized_input - knee) / (1.0 - knee), 0.0, 1.0);
        normalized_output = kmb_hermite(
            t,
            knee,
            1.0 - knee,
            normalized_target,
            0.0
        );
    }
    return kmb_clamp(
        kmb_hdr_pq_eotf(kmb_clamp(normalized_output * source_peak_code, 0.0, 1.0)),
        0.0,
        target_peak_nits
    );
}

static double kmb_bt709_oetf(double linear) {
    const double value = kmb_clamp(linear, 0.0, 1.0);
    return value < 0.018 ? 4.5 * value : 1.099 * pow(value, 0.45) - 0.099;
}

static KmbRgb kmb_bt2020_to_bt709(KmbRgb input) {
    const KmbRgb output = {
        1.660491 * input.red - 0.587641 * input.green - 0.072850 * input.blue,
        -0.124550 * input.red + 1.132900 * input.green - 0.008349 * input.blue,
        -0.018151 * input.red - 0.100579 * input.green + 1.118730 * input.blue,
    };
    return output;
}

static int kmb_is_in_unit_gamut(KmbRgb color) {
    const double epsilon = 1.0e-7;
    return color.red >= -epsilon && color.red <= 1.0 + epsilon &&
        color.green >= -epsilon && color.green <= 1.0 + epsilon &&
        color.blue >= -epsilon && color.blue <= 1.0 + epsilon;
}

static KmbRgb kmb_clamp_rgb(KmbRgb color) {
    color.red = kmb_clamp(color.red, 0.0, 1.0);
    color.green = kmb_clamp(color.green, 0.0, 1.0);
    color.blue = kmb_clamp(color.blue, 0.0, 1.0);
    return color;
}

static KmbIctcp kmb_bt2020_to_ictcp(KmbRgb input, double peak_nits) {
    const double l = (1688.0 * input.red + 2146.0 * input.green + 262.0 * input.blue) / 4096.0;
    const double m = (683.0 * input.red + 2951.0 * input.green + 462.0 * input.blue) / 4096.0;
    const double s = (99.0 * input.red + 309.0 * input.green + 3688.0 * input.blue) / 4096.0;
    const double lp = kmb_hdr_pq_oetf(fmax(l, 0.0) * peak_nits);
    const double mp = kmb_hdr_pq_oetf(fmax(m, 0.0) * peak_nits);
    const double sp = kmb_hdr_pq_oetf(fmax(s, 0.0) * peak_nits);
    const KmbIctcp output = {
        0.5 * lp + 0.5 * mp,
        (6610.0 * lp - 13613.0 * mp + 7003.0 * sp) / 4096.0,
        (17933.0 * lp - 17390.0 * mp - 543.0 * sp) / 4096.0,
    };
    return output;
}

static KmbRgb kmb_ictcp_to_bt2020(KmbIctcp input, double peak_nits) {
    const double lp = input.intensity + 0.008609037 * input.tritan + 0.111029625 * input.protan;
    const double mp = input.intensity - 0.008609037 * input.tritan - 0.111029625 * input.protan;
    const double sp = input.intensity + 0.560031336 * input.tritan - 0.320627175 * input.protan;
    const double l = kmb_hdr_pq_eotf(kmb_clamp(lp, 0.0, 1.0)) / peak_nits;
    const double m = kmb_hdr_pq_eotf(kmb_clamp(mp, 0.0, 1.0)) / peak_nits;
    const double s = kmb_hdr_pq_eotf(kmb_clamp(sp, 0.0, 1.0)) / peak_nits;
    const KmbRgb output = {
        3.43660669 * l - 2.50645212 * m + 0.06984542 * s,
        -0.79132956 * l + 1.98360045 * m - 0.19227090 * s,
        -0.02594990 * l - 0.09891371 * m + 1.12486361 * s,
    };
    return output;
}

static KmbRgb kmb_gamut_map(KmbRgb input, double peak_nits) {
    const KmbRgb direct = kmb_bt2020_to_bt709(input);
    KmbIctcp source;
    KmbIctcp candidate_ictcp;
    KmbRgb best;
    double low = 0.0;
    double high = 1.0;
    int step = 0;
    if (kmb_is_in_unit_gamut(direct)) return kmb_clamp_rgb(direct);

    source = kmb_bt2020_to_ictcp(input, peak_nits);
    candidate_ictcp = source;
    candidate_ictcp.tritan = 0.0;
    candidate_ictcp.protan = 0.0;
    best = kmb_bt2020_to_bt709(kmb_ictcp_to_bt2020(candidate_ictcp, peak_nits));
    for (step = 0; step < KMB_GAMUT_SEARCH_STEPS; ++step) {
        const double scale = (low + high) * 0.5;
        const KmbRgb candidate = kmb_bt2020_to_bt709(
            kmb_ictcp_to_bt2020(
                (KmbIctcp){
                    source.intensity,
                    source.tritan * scale,
                    source.protan * scale,
                },
                peak_nits
            )
        );
        if (kmb_is_in_unit_gamut(candidate)) {
            best = candidate;
            low = scale;
        } else {
            high = scale;
        }
    }
    return kmb_clamp_rgb(best);
}

static float kmb_lookup_1d(const float *table, double input) {
    const double position = kmb_clamp(input, 0.0, 1.0) * (KMB_TRANSFER_LUT_SIZE - 1);
    const int lower = (int)position;
    const int upper = lower < KMB_TRANSFER_LUT_SIZE - 1 ? lower + 1 : lower;
    const double amount = position - lower;
    return (float)(table[lower] * (1.0 - amount) + table[upper] * amount);
}

static size_t kmb_gamut_index(int red, int green, int blue, int channel) {
    return (size_t)((((blue * KMB_GAMUT_LUT_EDGE) + green) * KMB_GAMUT_LUT_EDGE + red) *
        KMB_GAMUT_LUT_CHANNELS + channel);
}

static KmbRgb kmb_lookup_gamut(const KmbHdrColorTransform *transform, KmbRgb input) {
    const double red_position = kmb_clamp(input.red, 0.0, 1.0) * (KMB_GAMUT_LUT_EDGE - 1);
    const double green_position = kmb_clamp(input.green, 0.0, 1.0) * (KMB_GAMUT_LUT_EDGE - 1);
    const double blue_position = kmb_clamp(input.blue, 0.0, 1.0) * (KMB_GAMUT_LUT_EDGE - 1);
    const int red0 = (int)red_position;
    const int green0 = (int)green_position;
    const int blue0 = (int)blue_position;
    const int red1 = red0 < KMB_GAMUT_LUT_EDGE - 1 ? red0 + 1 : red0;
    const int green1 = green0 < KMB_GAMUT_LUT_EDGE - 1 ? green0 + 1 : green0;
    const int blue1 = blue0 < KMB_GAMUT_LUT_EDGE - 1 ? blue0 + 1 : blue0;
    const double red_amount = red_position - red0;
    const double green_amount = green_position - green0;
    const double blue_amount = blue_position - blue0;
    KmbRgb result = {0.0, 0.0, 0.0};
    int bz = 0;
    int gy = 0;
    int rx = 0;
    int channel = 0;
    double *channels[3] = {&result.red, &result.green, &result.blue};
    for (bz = 0; bz < 2; ++bz) {
        const int blue = bz ? blue1 : blue0;
        const double blue_weight = bz ? blue_amount : 1.0 - blue_amount;
        for (gy = 0; gy < 2; ++gy) {
            const int green = gy ? green1 : green0;
            const double green_weight = gy ? green_amount : 1.0 - green_amount;
            for (rx = 0; rx < 2; ++rx) {
                const int red = rx ? red1 : red0;
                const double red_weight = rx ? red_amount : 1.0 - red_amount;
                const double weight = red_weight * green_weight * blue_weight;
                for (channel = 0; channel < 3; ++channel) {
                    *channels[channel] +=
                        transform->gamut_lut[kmb_gamut_index(red, green, blue, channel)] * weight;
                }
            }
        }
    }
    return kmb_clamp_rgb(result);
}

static void kmb_initialize_gamut_lut(KmbHdrColorTransform *transform) {
    const double denominator = KMB_GAMUT_LUT_EDGE - 1.0;
    int blue = 0;
    int green = 0;
    int red = 0;
    for (blue = 0; blue < KMB_GAMUT_LUT_EDGE; ++blue) {
        for (green = 0; green < KMB_GAMUT_LUT_EDGE; ++green) {
            for (red = 0; red < KMB_GAMUT_LUT_EDGE; ++red) {
                const KmbRgb mapped = kmb_gamut_map(
                    (KmbRgb){red / denominator, green / denominator, blue / denominator},
                    transform->target_white_nits
                );
                transform->gamut_lut[kmb_gamut_index(red, green, blue, 0)] = (float)mapped.red;
                transform->gamut_lut[kmb_gamut_index(red, green, blue, 1)] = (float)mapped.green;
                transform->gamut_lut[kmb_gamut_index(red, green, blue, 2)] = (float)mapped.blue;
            }
        }
    }
}

KmbHdrColorTransform *kmb_hdr_color_transform_create(
    KmbHdrTransfer transfer,
    double source_peak_nits,
    double target_white_nits
) {
    KmbHdrColorTransform *transform = NULL;
    double system_gamma = 0.0;
    int index = 0;
    if ((transfer != KMB_HDR_TRANSFER_PQ && transfer != KMB_HDR_TRANSFER_HLG) ||
        !isfinite(source_peak_nits) || source_peak_nits <= 0.0 || source_peak_nits > KMB_PQ_MAX_NITS ||
        !isfinite(target_white_nits) || target_white_nits <= 0.0 || target_white_nits > source_peak_nits) {
        return NULL;
    }
    system_gamma = 1.2 + 0.42 * log10(source_peak_nits / 1000.0);
    transform = (KmbHdrColorTransform *)calloc(1, sizeof(*transform));
    if (transform == NULL) return NULL;
    transform->transfer = transfer;
    transform->source_peak_nits = source_peak_nits;
    transform->target_white_nits = target_white_nits;
    for (index = 0; index < KMB_TRANSFER_LUT_SIZE; ++index) {
        const double normalized = index / (double)(KMB_TRANSFER_LUT_SIZE - 1);
        const double source_nits = normalized * source_peak_nits;
        transform->transfer_lut[index] = (float)(
            transfer == KMB_HDR_TRANSFER_PQ
                ? kmb_hdr_pq_eotf(normalized)
                : kmb_hdr_hlg_inverse_oetf(normalized)
        );
        transform->tone_lut[index] = (float)(
            kmb_hdr_tone_map_bt2390(source_nits, source_peak_nits, target_white_nits) /
            target_white_nits
        );
        transform->hlg_ootf_scale_lut[index] = (float)(
            normalized <= 0.0
                ? 0.0
                : source_peak_nits * pow(normalized, fmax(system_gamma - 1.0, 0.0))
        );
        transform->bt709_oetf_lut[index] = (float)kmb_bt709_oetf(normalized);
    }
    kmb_initialize_gamut_lut(transform);
    return transform;
}

void kmb_hdr_color_transform_free(KmbHdrColorTransform **transform) {
    if (transform == NULL || *transform == NULL) return;
    free(*transform);
    *transform = NULL;
}

int kmb_hdr_color_transform_pixel(
    const KmbHdrColorTransform *transform,
    float source_red,
    float source_green,
    float source_blue,
    float *output_red,
    float *output_green,
    float *output_blue
) {
    KmbRgb source_linear;
    KmbRgb mapped;
    double luminance_nits = 0.0;
    double mapped_luminance = 0.0;
    double scale = 0.0;
    if (transform == NULL || output_red == NULL || output_green == NULL || output_blue == NULL) return 0;

    source_linear.red = kmb_lookup_1d(transform->transfer_lut, source_red);
    source_linear.green = kmb_lookup_1d(transform->transfer_lut, source_green);
    source_linear.blue = kmb_lookup_1d(transform->transfer_lut, source_blue);
    if (transform->transfer == KMB_HDR_TRANSFER_HLG) {
        const double scene_luminance = kmb_clamp(
            0.2627 * source_linear.red + 0.6780 * source_linear.green + 0.0593 * source_linear.blue,
            0.0,
            1.0
        );
        const double ootf_scale = kmb_lookup_1d(transform->hlg_ootf_scale_lut, scene_luminance);
        source_linear.red *= ootf_scale;
        source_linear.green *= ootf_scale;
        source_linear.blue *= ootf_scale;
    }
    luminance_nits = fmax(
        0.2627 * source_linear.red + 0.6780 * source_linear.green + 0.0593 * source_linear.blue,
        0.0
    );
    mapped_luminance = kmb_lookup_1d(
        transform->tone_lut,
        luminance_nits / transform->source_peak_nits
    ) * transform->target_white_nits;
    scale = luminance_nits > 1.0e-9 ? mapped_luminance / luminance_nits : 0.0;
    source_linear.red = kmb_clamp(
        source_linear.red * scale / transform->target_white_nits,
        0.0,
        1.0
    );
    source_linear.green = kmb_clamp(
        source_linear.green * scale / transform->target_white_nits,
        0.0,
        1.0
    );
    source_linear.blue = kmb_clamp(
        source_linear.blue * scale / transform->target_white_nits,
        0.0,
        1.0
    );
    mapped = kmb_lookup_gamut(transform, source_linear);
    *output_red = kmb_lookup_1d(transform->bt709_oetf_lut, mapped.red);
    *output_green = kmb_lookup_1d(transform->bt709_oetf_lut, mapped.green);
    *output_blue = kmb_lookup_1d(transform->bt709_oetf_lut, mapped.blue);
    return 1;
}

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
) {
    int y = 0;
    if (transform == NULL || green == NULL || blue == NULL || red == NULL ||
        green_stride_bytes <= 0 || blue_stride_bytes <= 0 || red_stride_bytes <= 0 ||
        width <= 0 || height <= 0) {
        return 0;
    }
    for (y = 0; y < height; ++y) {
        float *green_row = (float *)((uint8_t *)green + y * green_stride_bytes);
        float *blue_row = (float *)((uint8_t *)blue + y * blue_stride_bytes);
        float *red_row = (float *)((uint8_t *)red + y * red_stride_bytes);
        int x = 0;
        for (x = 0; x < width; ++x) {
            float output_red = 0.0f;
            float output_green = 0.0f;
            float output_blue = 0.0f;
            if (!kmb_hdr_color_transform_pixel(
                    transform,
                    red_row[x],
                    green_row[x],
                    blue_row[x],
                    &output_red,
                    &output_green,
                    &output_blue
                )) {
                return 0;
            }
            red_row[x] = output_red;
            green_row[x] = output_green;
            blue_row[x] = output_blue;
        }
    }
    return 1;
}
