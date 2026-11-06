// File/layer: core/include/ecuio/socketcan.h | C ABI, public interface
// Testing: UNIT: SocketCanTest (test_socketcan); HARNESS: VcanTest
//          (test_vcan)

#ifndef ECUIO_SOCKETCAN_H
#define ECUIO_SOCKETCAN_H

#include "ecuio/core.h"    /* ecuio_status_t */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * CAN-interface discovery for socketcan:// (ADR-0032 section 7), Linux only.
 *
 * Lists the network interfaces whose link kind is "can" (a CAN
 * controller: candleLight and CANable through gs_usb, PEAK, Kvaser, an
 * MCP2515) or "vcan" (Linux's virtual bus), as rtnetlink reports them,
 * with no privileges. Configures nothing: the user brings an interface up
 * with its bit rate (and, for listening, listen-only mode) through
 * `ip link`. Text is ASCII, as interface names are.
 */
#define ECUIO_CAN_NAME_LEN   16   /* IFNAMSIZ, NUL included */
#define ECUIO_CAN_KIND_LEN   16

/* flags */
#define ECUIO_CAN_UP           0x00000001u   /* the interface is up */
#define ECUIO_CAN_LISTEN_ONLY  0x00000002u   /* a controller in
                                                listen-only mode */
#define ECUIO_CAN_FD           0x00000004u   /* takes CAN FD frames
                                                (MTU 72) */
#define ECUIO_CAN_MODES_KNOWN  0x00000008u   /* modes_supported is the
                                                driver's (the kernel says
                                                from Linux 6.0) */

/* state: the controller's, as the kernel reports it; append-only */
#define ECUIO_CAN_STATE_UNKNOWN        0u    /* vcan, or none reported */
#define ECUIO_CAN_STATE_ERROR_ACTIVE   1u
#define ECUIO_CAN_STATE_ERROR_WARNING  2u
#define ECUIO_CAN_STATE_ERROR_PASSIVE  3u
#define ECUIO_CAN_STATE_BUS_OFF        4u
#define ECUIO_CAN_STATE_STOPPED        5u
#define ECUIO_CAN_STATE_SLEEPING       6u

/* modes and modes_supported: the kernel's controller-mode bits
 * (CAN_CTRLMODE_*), passed on whole; these are the ones named */
#define ECUIO_CAN_MODE_LOOPBACK         0x001u
#define ECUIO_CAN_MODE_LISTEN_ONLY      0x002u
#define ECUIO_CAN_MODE_TRIPLE_SAMPLING  0x004u
#define ECUIO_CAN_MODE_ONE_SHOT         0x008u
#define ECUIO_CAN_MODE_BERR_REPORTING   0x010u
#define ECUIO_CAN_MODE_FD               0x020u
#define ECUIO_CAN_MODE_PRESUME_ACK      0x040u
#define ECUIO_CAN_MODE_FD_NON_ISO       0x080u
#define ECUIO_CAN_MODE_CC_LEN8_DLC      0x100u

/* POD, 64 bytes, no hidden padding; pinned by the assertions below */
typedef struct ecuio_can_interface {
    char name[ECUIO_CAN_NAME_LEN];   /* what follows socketcan://: "can0" */
    char kind[ECUIO_CAN_KIND_LEN];   /* "can" or "vcan" */
    uint32_t flags;                  /* ECUIO_CAN_* */
    uint32_t bitrate;                /* bit/s a controller was brought up
                                        at; 0 for vcan, or when unknown */
    uint32_t state;                  /* ECUIO_CAN_STATE_* */
    uint32_t modes;                  /* ECUIO_CAN_MODE_* on; 0 for vcan */
    uint32_t modes_supported;        /* the driver's, with
                                        ECUIO_CAN_MODES_KNOWN; else 0 */
    uint32_t reserved[3];            /* zero; ABI growth space */
} ecuio_can_interface_t;

#if defined(__cplusplus)
static_assert(sizeof(ecuio_can_interface_t) == 64, "ecuio_can_interface_t size is ABI");
static_assert(offsetof(ecuio_can_interface_t, flags) == 32, "can interface flags offset is ABI");
static_assert(offsetof(ecuio_can_interface_t, state) == 40, "can interface state offset is ABI");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(ecuio_can_interface_t) == 64, "ecuio_can_interface_t size is ABI");
_Static_assert(offsetof(ecuio_can_interface_t, flags) == 32, "can interface flags offset is ABI");
_Static_assert(offsetof(ecuio_can_interface_t, state) == 40, "can interface state offset is ABI");
#endif

/*
 * Words for a state and a mode, as ip link spells them: "error-active",
 * "bus-off"; "listen-only", "one-shot". "" for ECUIO_CAN_STATE_UNKNOWN, a
 * state past the list, or anything but one named mode bit. Every
 * platform.
 */
const char* ecuio_can_state_name(uint32_t state);
const char* ecuio_can_mode_name(uint32_t mode);

/*
 * The machine's CAN interfaces.
 *
 * CONTRACTS:
 * - *count is the number found; the first min(*count, capacity) are
 *   copied into list. Capacity 0 (list may then be NULL) asks for the
 *   count alone
 * - In the kernel's order (by interface index)
 * - ECUIO_OK with *count = 0 when there are none, and on every platform
 *   but Linux
 * - NULL count, or NULL list with a capacity: ECUIO_ERR_NULL_ARGUMENT
 * - ECUIO_ERR_LOAD_FAILED when the kernel's interface list can't be read
 *   (ecuio_last_error_detail is not set: this isn't a connect)
 * - Opens no CAN socket and changes nothing
 */
ecuio_status_t ecuio_can_interfaces(ecuio_can_interface_t* list,
                                    uint32_t capacity, uint32_t* count);

#ifdef __cplusplus
}
#endif

#endif /* ECUIO_SOCKETCAN_H */
