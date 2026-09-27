/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "kmedia_bridge_hevc.h"

#include <assert.h>
#include <string.h>

int main(void) {
    uint8_t configuration[44] = {1};
    assert(!kmb_hevc_configuration_complete(NULL, 23));
    assert(!kmb_hevc_configuration_complete(configuration, 22));
    assert(!kmb_hevc_configuration_complete(configuration, 23));
    configuration[22] = 3;
    for (unsigned int i = 0; i < 3; i++) {
        uint8_t *array = configuration + 23 + i * 7;
        array[0] = (uint8_t)(0x80 | (32 + i));
        array[2] = 1;
        array[4] = 2;
        array[5] = (uint8_t)((32 + i) << 1);
        array[6] = 1;
    }
    assert(kmb_hevc_configuration_complete(configuration, sizeof(configuration)));
    /* Never accept a truncated array or a partial VPS/SPS/PPS configuration. */
    for (size_t size = 0; size < sizeof(configuration); size++) {
        assert(!kmb_hevc_configuration_complete(configuration, size));
    }
    configuration[22] = 2;
    assert(!kmb_hevc_configuration_complete(configuration, 37));
    configuration[22] = 3;
    configuration[42] = 32 << 1;
    assert(!kmb_hevc_configuration_complete(configuration, sizeof(configuration)));
    configuration[42] = 34 << 1;
    configuration[40] = 0xff;
    assert(!kmb_hevc_configuration_complete(configuration, sizeof(configuration)));

    /* Closing a partially prepared source must free every buffered packet. */
    KmbHevcPreparation preparation = {0};
    preparation.prefix[0] = av_packet_alloc();
    assert(preparation.prefix[0] != NULL);
    assert(av_new_packet(preparation.prefix[0], 8) == 0);
    preparation.count = 1;
    kmb_hevc_close(&preparation);
    assert(preparation.prefix[0] == NULL && preparation.count == 0 && preparation.next == 0);
    kmb_hevc_close(&preparation);
    return 0;
}
