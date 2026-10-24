// File/layer: core/include/ecuio/surrogate_protocol.h | shared app<->surrogate contract
// Testing: structural — compiled by BOTH x64 client and x86 surrogate, so
//          layout drift is impossible WITHIN a tree. Across trees it fails
//          loudly: byte-mode pipe reads return the WRITER's struct size.
//          A changed value in a same-sized struct is caught at OP_OPEN by
//          the fingerprint (J2534SurrogateTest.MismatchedFingerprintRefusesOpenBeforeVendor).

#ifndef ECUIO_SURROGATE_PROTOCOL_H
#define ECUIO_SURROGATE_PROTOCOL_H

#include <stdint.h>

#include "ecuio/j2534_ptypes.h"

/* ---- Wire limits ---- */
#define ECUIO_SUR_MSG_MAX ECUIO_PT_WRITE_MAX  /* bytes of one written or injected
                                                 message's Data; a read's frames
                                                 are whole ecuio_pt_rx_t */
#define ECUIO_SUR_IOCTL_DATA_MAX (ECUIO_PT_SCONFIG_MAX_PARAMS * 8)  /* ioctl input
                                           (whole SCONFIG pairs), and reply
                                           payload */
#define ECUIO_SUR_FILTER_PATTERN_MAX 12    /* mask/pattern/flow-control bytes */
#define ECUIO_SUR_MAX_FRAMES_PER_READ 16   /* frames in one OP_READ reply */
#define ECUIO_SUR_TEXT_MAX 80              /* one vendor text: PassThruGetLastError's,
                                              or one PassThruReadVersion string */
#define ECUIO_SUR_TEXT_FIELDS 3            /* texts in one reply */

/* Request opcodes — append-only, never renumber.
 *
 * One request, one response, paired by request_id. The child answers
 * every request with a ecuio_sur_response_t whose status is the
 * vendor's J2534 status, or STATUS_FAILED when the child refused the
 * request before calling the vendor (a length over a wire limit, a
 * non-whitelisted ioctl, an unknown opcode). A non-success status never
 * ends the child (ADR-0014); only a broken pipe or OP_SHUTDOWN does.
 *
 *   OPEN          u.open: the client's ECUIO_SUR_FINGERPRINT, a hash of
 *                 this header and j2534_ptypes.h generated at configure
 *                 time. The child refuses a fingerprint not its own
 *                 with STATUS_FAILED before calling the vendor, so two
 *                 trees built from different headers never talk.
 *                 Otherwise PassThruOpen. Reply: status, and aux = the
 *                 child's fingerprint, which the client checks too; the
 *                 device ID stays in the child.
 *   CONNECT       u.connect. PassThruConnect on that device.
 *                 Reply: handle_or_channel = the channel ID.
 *   IOCTL         u.ioctl. PassThruIoctl, whitelisted IDs only
 *                 (READ_VBATT, the three CLEARs, GET/SET_CONFIG).
 *                 Reply: data/data_len (READ_VBATT's 4 bytes, or
 *                 GET_CONFIG's re-flattened pairs).
 *   WRITE         u.write. One PassThruWriteMsgs message.
 *                 Reply: aux = messages the vendor reports written.
 *   READ          u.read. PassThruReadMsgs for up to 16 messages on
 *                 the child's open channel (no channel field). Reply:
 *                 num_frames, then that many ecuio_pt_rx_t after the
 *                 response (see below): the messages the vendor returned
 *                 under NOERROR or ERR_TIMEOUT (the count-trust rule in
 *                 j2534_ptypes.h; a conforming vendor pairs ERR_TIMEOUT
 *                 with a short batch), with indications already
 *                 skipped (ecuio_pt_is_indication; a batch of nothing
 *                 else is read again with what's left of the timeout),
 *                 plus the vendor status verbatim. No open channel:
 *                 NOERROR with no frames.
 *   SHUTDOWN      no fields. Reply NOERROR, then the child disconnects,
 *                 closes the device, unloads the DLL and exits 0.
 *   DISCONNECT    u.disconnect. PassThruDisconnect. Reply: status only.
 *   INJECT        u.inject. Test-only: hands the frame to the loaded
 *                 DLL's InjectFrame export (the fake's). STATUS_FAILED
 *                 when the DLL has none.
 *   START_FILTER  u.start_filter. PassThruStartMsgFilter.
 *                 Reply: handle_or_channel = the filter ID.
 *   STOP_FILTER   u.stop_filter. PassThruStopMsgFilter. Reply: status
 *                 only.
 *   GET_LAST_ERROR no fields. PassThruGetLastError. Reply: text[0], the
 *                 vendor's description of its last failed call,
 *                 NUL-terminated; status the vendor's, or
 *                 ERR_NOT_SUPPORTED when the DLL doesn't export it.
 *   READ_VERSION  no fields. PassThruReadVersion on the open device.
 *                 Reply: text[0], [1] and [2], the firmware, DLL and
 *                 API versions, each NUL-terminated; status the
 *                 vendor's, or ERR_NOT_SUPPORTED when the DLL doesn't
 *                 export it. */
#define ECUIO_SUR_OP_OPEN        1
#define ECUIO_SUR_OP_CONNECT     2
#define ECUIO_SUR_OP_IOCTL       3
#define ECUIO_SUR_OP_WRITE       4
#define ECUIO_SUR_OP_READ        5
#define ECUIO_SUR_OP_SHUTDOWN    6
#define ECUIO_SUR_OP_DISCONNECT 7
#define ECUIO_SUR_OP_INJECT      8
#define ECUIO_SUR_OP_START_FILTER 9
#define ECUIO_SUR_OP_STOP_FILTER   10
#define ECUIO_SUR_OP_GET_LAST_ERROR 11
#define ECUIO_SUR_OP_READ_VERSION  12

typedef struct ecuio_sur_request {
    uint32_t request_id;   /* client-assigned, increasing; echoed back */
    uint32_t opcode;       /* ECUIO_SUR_OP_* */
    union {
        struct { uint32_t fingerprint; } open;
        struct { uint32_t protocol_id; uint32_t flags; uint32_t baud; } connect;
        struct { uint32_t channel_id; } disconnect;
        /* input: flattened SCONFIG pairs (u32 Parameter, u32 Value, LE)
         * for GET/SET_CONFIG; input_len 0 for NULL-input ioctls. */
        struct { uint32_t channel_id; uint32_t ioctl_id;
                 uint8_t input[ECUIO_SUR_IOCTL_DATA_MAX];
                 uint32_t input_len; } ioctl;
        struct { uint32_t channel_id;
                 uint8_t data[ECUIO_SUR_MSG_MAX];
                 uint32_t data_len; uint32_t tx_flags;
                 uint32_t timeout_ms; } write;
        /* The vendor's ReadMsgs timeout. The client waits that plus
         * 500 ms for the reply before it calls the child hung. */
        struct { uint32_t timeout_ms; } read;
        struct { uint8_t data[ECUIO_SUR_MSG_MAX]; uint32_t len;
                 uint32_t timestamp; uint32_t rx_flags; } inject;
        struct { uint32_t channel_id; uint32_t filter_type; uint32_t tx_flags;
                 uint8_t mask[ECUIO_SUR_FILTER_PATTERN_MAX];
                 uint8_t pattern[ECUIO_SUR_FILTER_PATTERN_MAX];
                 uint8_t flow_control[ECUIO_SUR_FILTER_PATTERN_MAX];
                 uint32_t mask_len; uint32_t pattern_len; uint32_t fc_len;
                 } start_filter;
        struct { uint32_t channel_id; uint32_t filter_id; } stop_filter;
    } u;
} ecuio_sur_request_t;

/* Every reply is one ecuio_sur_response_t followed by num_frames
 * ecuio_pt_rx_t (j2534_ptypes.h): the vendor's messages whole, CAN ID
 * in front, stamp untranslated. Only a READ's reply carries frames, so
 * a reply costs the frames it holds, not ECUIO_SUR_MAX_FRAMES_PER_READ
 * of them. Fields an opcode doesn't use are zero. */
typedef struct ecuio_sur_response {
    uint32_t request_id;         /* the request's, for pairing */
    int32_t  status;             /* vendor J2534 status, or STATUS_FAILED */
    uint32_t handle_or_channel;  /* CONNECT: channel ID; START_FILTER: filter ID */
    uint32_t aux;                /* OPEN: the child's fingerprint; WRITE: messages written */
    /* GET_LAST_ERROR: [0]; READ_VERSION: all three. The client
     * terminates each. */
    char     text[ECUIO_SUR_TEXT_FIELDS][ECUIO_SUR_TEXT_MAX];
    uint8_t  data[ECUIO_SUR_IOCTL_DATA_MAX];   /* IOCTL reply payload */
    uint32_t data_len;           /* IOCTL; the client rejects > the limit */
    uint32_t num_frames;         /* READ: the ecuio_pt_rx_t that follow; the
                                    client rejects > the limit, and any on
                                    another opcode's reply */
} ecuio_sur_response_t;

#endif /* ECUIO_SURROGATE_PROTOCOL_H */
