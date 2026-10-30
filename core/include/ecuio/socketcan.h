// File/layer: core/include/ecuio/socketcan.h | C ABI, public interface
// Testing: UNIT: SocketCanTest (test_socketcan): the contract's
//          arguments on every platform, and the interfaces read from
//          canned netlink replies; HARNESS: VcanTest (Linux): vcan0
//          listed, up, with its MTU

#ifndef ECUIO_SOCKETCAN_H
#define ECUIO_SOCKETCAN_H

#include "ecuio/core.h"    /* ecuio_status_t */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * CAN-interface discovery for socketcan:// (ADR-0032 §7), Linux only.
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

/* POD, 64 bytes, no hidden padding; pinned by the assertions below */
typedef struct ecuio_can_interface {
    char name[ECUIO_CAN_NAME_LEN];   /* what follows socketcan://: "can0" */
    char kind[ECUIO_CAN_KIND_LEN];   /* "can" or "vcan" */
    uint32_t flags;                  /* ECUIO_CAN_* */
    uint32_t bitrate;                /* bit/s a controller was brought up
                                        at; 0 for vcan, or when unknown */
    uint32_t reserved[6];            /* zero; ABI growth space */
} ecuio_can_interface_t;

#if defined(__cplusplus)
static_assert(sizeof(ecuio_can_interface_t) == 64, "ecuio_can_interface_t size is ABI");
static_assert(offsetof(ecuio_can_interface_t, flags) == 32, "can interface flags offset is ABI");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(ecuio_can_interface_t) == 64, "ecuio_can_interface_t size is ABI");
_Static_assert(offsetof(ecuio_can_interface_t, flags) == 32, "can interface flags offset is ABI");
#endif

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
