// File/layer: core/include/ecuio/serial.h | C ABI, public interface
// Testing: UNIT — SerialPortsTest (test_elm327): the order, the dedupe and
//          the copy over made-up ports, the /dev names listed, and the
//          contract against the machine's own ports; the live list in
//          Settings is manual smoke

#ifndef ECUIO_SERIAL_H
#define ECUIO_SERIAL_H

#include "ecuio/core.h"    /* ecuio_status_t */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Serial-port discovery for elm327:// (ADR-0026 §7).
 *
 * Lists the ports the OS knows and opens none of them: an ELM327's
 * commands could disturb whatever is behind a port that isn't an
 * adapter, so the user picks the port, as with a J2534 interface.
 *   Windows: the values of HKLM\HARDWARE\DEVICEMAP\SERIALCOMM
 *   macOS:   /dev/cu.*
 *   Linux and other POSIX systems: /dev/ttyUSB<n>, /dev/ttyACM<n> and
 *            /dev/rfcomm<n>
 * A paired classic-Bluetooth adapter is a serial port on all three.
 * Text is UTF-8.
 */
#define ECUIO_SERIAL_NAME_LEN    128   /* elm327://'s longest port name,
                                          NUL included */
#define ECUIO_SERIAL_DEVICE_LEN  128

/* POD, 288 bytes, no hidden padding; pinned by the assertions below */
typedef struct ecuio_serial_port {
    char name[ECUIO_SERIAL_NAME_LEN];       /* what follows elm327://:
                                               "COM5", "/dev/ttyUSB0" */
    char device[ECUIO_SERIAL_DEVICE_LEN];   /* Windows: the driver's device,
                                               SERIALCOMM's value name
                                               ("\Device\BthModem0" for
                                               Bluetooth); "" elsewhere,
                                               and when too long */
    uint32_t flags;                         /* 0 */
    uint32_t reserved[7];                   /* zero; ABI growth space */
} ecuio_serial_port_t;

#if defined(__cplusplus)
static_assert(sizeof(ecuio_serial_port_t) == 288, "ecuio_serial_port_t size is ABI");
static_assert(offsetof(ecuio_serial_port_t, flags) == 256, "serial port flags offset is ABI");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(ecuio_serial_port_t) == 288, "ecuio_serial_port_t size is ABI");
_Static_assert(offsetof(ecuio_serial_port_t, flags) == 256, "serial port flags offset is ABI");
#endif

/*
 * The machine's serial ports.
 *
 * CONTRACTS:
 * - *count is the number found; the first min(*count, capacity) are
 *   copied into ports. Capacity 0 (ports may then be NULL) asks for the
 *   count alone
 * - In name order, a run of digits by its value (COM2 before COM10),
 *   each name once
 * - Only names elm327:// takes are listed (COM1 to COM255 on Windows,
 *   a /dev/ path elsewhere)
 * - ECUIO_OK with *count = 0 when there are none (not an error)
 * - NULL count, or NULL ports with a capacity: ECUIO_ERR_NULL_ARGUMENT
 * - Opens no port
 */
ecuio_status_t ecuio_serial_ports(ecuio_serial_port_t* ports,
                                  uint32_t capacity, uint32_t* count);

#ifdef __cplusplus
}
#endif

#endif /* ECUIO_SERIAL_H */
