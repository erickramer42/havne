/* File/layer: tests/test_c_abi_frames.c | strict C part of test_c_abi
 * Testing: UNIT: VIN decode, streams, the J2534 device list and the pull queue, compile and link, run as c_abi and c_abi_shared
 */

#include "ecuio/conditions.h"
#include "ecuio/queue.h"
#include "ecuio/obdb.h"
#include "ecuio/recording.h"
#include "ecuio/internal.h"
#include "ecuio/surrogate_protocol.h"
#include "ecuio/j2534_surrogate.h"
#include "ecuio/j2534.h"
#include "ecuio/stream.h"
#include "ecuio/serial.h"
#include "ecuio/plugin.h"
#include "ecuio/vin.h"
#include "ecuio/sim.h"
#include "ecuio/socketcan.h"
#include "ecuio/profile.h"
#include "ecuio/signal_sets.h"
#include "ecuio/signals.h"
#include "ecuio/core.h"
#include "ecuio/version.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "test_c_abi_parts.h"

int c_abi_frames(void)
{
    ecuio_frame_t frame;
    ecuio_stream_callbacks_t stream_cb;
    ecuio_stream_t* stream = NULL;
    ecuio_vin_info_t vin_info;
    ecuio_queue_t* queue = NULL;
    ecuio_queue_value_t queue_value;
    uint32_t queue_count = 0;
    uint64_t queue_dropped = 0;
    frame.arbitration_id = 0x7FF;
    frame.channel_id = 0;
    frame.data_length = 8;
    frame.flags = ECUIO_FRAME_FLAG_EXTENDED;
    frame.reserved = 0;

    if (ecuio_vin_decode(NULL, 0, &vin_info) != ECUIO_ERR_NULL_ARGUMENT ||
        ecuio_vin_make(NULL)[0] != 0 || ecuio_vin_source()[0] == 0) {
        return 1;
    }
    memset(&stream_cb, 0, sizeof stream_cb);
    if (ecuio_stream_register("bt:x", &stream_cb, &stream) != ECUIO_ERR_NULL_ARGUMENT ||
        ecuio_stream_unregister(NULL) != ECUIO_ERR_NULL_ARGUMENT || stream != NULL) {
        return 1;
    }
    ecuio_stream_opened(NULL);
    ecuio_stream_received(NULL, NULL, 0);
    ecuio_stream_failed(NULL, NULL);
    ecuio_j2534_free_devices(NULL);
    /* The pull queue (ADR-0053 section 4): its NULL refusals */
    if (ecuio_queue_open(NULL, ECUIO_QUEUE_FRAMES, 1, &queue) !=
            ECUIO_ERR_NULL_ARGUMENT ||
        queue != NULL ||
        ecuio_queue_read_frames(NULL, &frame, 1, &queue_count, &queue_dropped,
                                0) != ECUIO_ERR_NULL_ARGUMENT ||
        ecuio_queue_read_values(NULL, &queue_value, 1, &queue_count,
                                &queue_dropped, 0) != ECUIO_ERR_NULL_ARGUMENT) {
        return 1;
    }
    ecuio_queue_close(NULL);
    return 0;
}
