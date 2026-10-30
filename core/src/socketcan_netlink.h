// File/layer: core/src/socketcan_netlink.h | internal, not ABI-exported;
//             socketcan_interfaces.cpp defines it, socketcan_transport.cpp
//             uses it
// C++17, Qt-free.
// Testing: HARNESS: VcanTest (Linux): vcan0 found, up, its MTU; the
//     attributes' parsing is socketcan_wire.h's, UNIT on every platform
//
// The kernel's list of network links over rtnetlink (RTM_GETLINK, a
// dump), which needs no privileges (ADR-0032 §2). A stack buffer and a
// socket per call: the worker calls it again when it reopens a socket,
// and allocates nothing.

#ifndef ECUIO_SOCKETCAN_NETLINK_H
#define ECUIO_SOCKETCAN_NETLINK_H

#include "socketcan_wire.h"

/* Calls visit(link, user) for each link the kernel lists, CAN or not.
 * False, with errno set, when the list couldn't be read. Off Linux, true
 * with no link. */
bool socketcan_read_links(void (*visit)(const socketcan_link& link, void* user),
                          void* user);

/* The link named `name`: true with *found false when there's none. */
bool socketcan_find_link(const char* name, socketcan_link* out, bool* found);

#endif /* ECUIO_SOCKETCAN_NETLINK_H */
