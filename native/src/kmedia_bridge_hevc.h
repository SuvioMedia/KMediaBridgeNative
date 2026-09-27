/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef KMEDIA_BRIDGE_HEVC_H
#define KMEDIA_BRIDGE_HEVC_H

#include <libavcodec/bsf.h>
#include <libavformat/avformat.h>

#define KMB_HEVC_PREFIX_PACKETS 256

typedef struct KmbHevcPreparation {
    AVBSFContext *filter;
    AVPacket *prefix[KMB_HEVC_PREFIX_PACKETS];
    unsigned int count;
    unsigned int next;
} KmbHevcPreparation;

int kmb_hevc_configuration_complete(const uint8_t *data, size_t size);
int kmb_hevc_prepare(AVFormatContext *input, int video_track, KmbHevcPreparation *state);
int kmb_hevc_read_frame(AVFormatContext *input, int video_track, KmbHevcPreparation *state, AVPacket *packet);
void kmb_hevc_discard_prefix(KmbHevcPreparation *state);
void kmb_hevc_close(KmbHevcPreparation *state);

#endif
