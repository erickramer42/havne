// File/layer: core/src/socketcan_interfaces.cpp | C++17, Qt-free; netlink on
//             Linux, an empty list elsewhere
// Testing: UNIT — SocketCanTest.Interfaces* (the arguments' contract on
//          every platform); HARNESS — VcanTest.InterfacesListVcan0 (Linux);
//          the attributes' parsing is socketcan_wire.h's, UNIT
//
// CAN-interface discovery (ecuio/socketcan.h, ADR-0032 §7) and the link
// lookup the SocketCAN transport checks an interface with
// (socketcan_netlink.h).

#include "ecuio/socketcan.h"
#include "ecuio/internal.h"
#include "socketcan_netlink.h"

#include <errno.h>
#include <string.h>

#if ECUIO_HAVE_SOCKETCAN
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <sys/socket.h>
#include <unistd.h>

#include <linux/can.h>
#include <linux/can/error.h>
#include <linux/can/netlink.h>
#include <linux/if_link.h>
#include <net/if.h>

/* socketcan_wire.h mirrors these so its rules test on every platform */
static_assert(SOCKETCAN_EFF_FLAG == CAN_EFF_FLAG, "CAN_EFF_FLAG");
static_assert(SOCKETCAN_RTR_FLAG == CAN_RTR_FLAG, "CAN_RTR_FLAG");
static_assert(SOCKETCAN_ERR_FLAG == CAN_ERR_FLAG, "CAN_ERR_FLAG");
static_assert(SOCKETCAN_SFF_MASK == CAN_SFF_MASK, "CAN_SFF_MASK");
static_assert(SOCKETCAN_EFF_MASK == CAN_EFF_MASK, "CAN_EFF_MASK");
static_assert(SOCKETCAN_FD_BRS == CANFD_BRS, "CANFD_BRS");
static_assert(SOCKETCAN_MTU == CAN_MTU, "CAN_MTU");
static_assert(SOCKETCAN_FD_MTU == CANFD_MTU, "CANFD_MTU");
static_assert(SOCKETCAN_ERR_CRTL == CAN_ERR_CRTL, "CAN_ERR_CRTL");
static_assert(SOCKETCAN_ERR_BUSOFF == CAN_ERR_BUSOFF, "CAN_ERR_BUSOFF");
static_assert(SOCKETCAN_ERR_RESTARTED == CAN_ERR_RESTARTED,
              "CAN_ERR_RESTARTED");
static_assert(SOCKETCAN_CTRLMODE_LISTENONLY == CAN_CTRLMODE_LISTENONLY,
              "CAN_CTRLMODE_LISTENONLY");
static_assert(SOCKETCAN_IFLA_IFNAME == IFLA_IFNAME, "IFLA_IFNAME");
static_assert(SOCKETCAN_IFLA_MTU == IFLA_MTU, "IFLA_MTU");
static_assert(SOCKETCAN_IFLA_LINKINFO == IFLA_LINKINFO, "IFLA_LINKINFO");
static_assert(SOCKETCAN_IFLA_INFO_KIND == IFLA_INFO_KIND, "IFLA_INFO_KIND");
static_assert(SOCKETCAN_IFLA_INFO_DATA == IFLA_INFO_DATA, "IFLA_INFO_DATA");
static_assert(SOCKETCAN_IFLA_CAN_BITTIMING == IFLA_CAN_BITTIMING,
              "IFLA_CAN_BITTIMING");
static_assert(SOCKETCAN_IFLA_CAN_CTRLMODE == IFLA_CAN_CTRLMODE,
              "IFLA_CAN_CTRLMODE");
/* NLA_TYPE_MASK is ~(NESTED | BYTEORDER) as an int: its low 16 bits */
static_assert(SOCKETCAN_NLA_TYPE_MASK == static_cast<uint16_t>(NLA_TYPE_MASK),
              "NLA_TYPE_MASK");
static_assert(SOCKETCAN_IFF_UP == IFF_UP, "IFF_UP");
static_assert(SOCKETCAN_IFINFOMSG_LEN == sizeof(struct ifinfomsg), "ifinfomsg");
static_assert(SOCKETCAN_IFNAME_MAX + 1 == IFNAMSIZ, "IFNAMSIZ");
#endif

bool socketcan_read_links(void (*visit)(const socketcan_link& link, void* user),
                          void* user)
{
#if ECUIO_HAVE_SOCKETCAN
    const int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (fd < 0) {
        return false;
    }
    struct {
        struct nlmsghdr nh;
        struct ifinfomsg ifi;
    } req;
    memset(&req, 0, sizeof req);
    req.nh.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg));
    req.nh.nlmsg_type = RTM_GETLINK;
    req.nh.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    req.nh.nlmsg_seq = 1;
    req.ifi.ifi_family = AF_UNSPEC;
    if (send(fd, &req, req.nh.nlmsg_len, 0) < 0) {
        const int err = errno;
        close(fd);
        errno = err;
        return false;
    }
    /* The kernel's dump comes in messages of up to a page or so each;
     * 32 KB holds any one of them, aligned for nlmsghdr */
    alignas(struct nlmsghdr) unsigned char buf[32768];
    for (;;) {
        const ssize_t n = recv(fd, buf, sizeof buf, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            const int err = errno;
            close(fd);
            errno = err;
            return false;
        }
        size_t len = static_cast<size_t>(n);
        for (struct nlmsghdr* nh = reinterpret_cast<struct nlmsghdr*>(buf);
             NLMSG_OK(nh, len); nh = NLMSG_NEXT(nh, len)) {
            if (nh->nlmsg_type == NLMSG_DONE) {
                close(fd);
                return true;
            }
            if (nh->nlmsg_type == NLMSG_ERROR) {
                const struct nlmsgerr* e =
                    static_cast<const struct nlmsgerr*>(NLMSG_DATA(nh));
                close(fd);
                errno = e->error < 0 ? -e->error : EIO;
                return false;
            }
            if (nh->nlmsg_type != RTM_NEWLINK) {
                continue;
            }
            socketcan_link link;
            if (socketcan_parse_link(
                    static_cast<const uint8_t*>(NLMSG_DATA(nh)),
                    NLMSG_PAYLOAD(nh, 0), &link)) {
                visit(link, user);
            }
        }
        if (n == 0) {
            close(fd);
            errno = EIO;
            return false;
        }
    }
#else
    (void)visit;
    (void)user;
    return true;
#endif
}

namespace {

struct find_state {
    const char* name;
    socketcan_link* out;
    bool found;
};

void find_visit(const socketcan_link& link, void* user)
{
    find_state* s = static_cast<find_state*>(user);
    if (!s->found && strcmp(link.name, s->name) == 0) {
        *s->out = link;
        s->found = true;
    }
}

struct list_state {
    ecuio_can_interface_t* list;
    uint32_t capacity;
    uint32_t count;
};

void list_visit(const socketcan_link& link, void* user)
{
    list_state* s = static_cast<list_state*>(user);
    ecuio_can_interface_t item;
    if (!socketcan_interface_from(link, &item)) {
        return;
    }
    if (s->count < s->capacity) {
        s->list[s->count] = item;
    }
    ++s->count;
}

} // namespace

bool socketcan_find_link(const char* name, socketcan_link* out, bool* found)
{
    find_state s{name, out, false};
    const bool ok = socketcan_read_links(find_visit, &s);
    *found = s.found;
    return ok;
}

extern "C" ecuio_status_t ecuio_can_interfaces(ecuio_can_interface_t* list,
                                               uint32_t capacity,
                                               uint32_t* count)
{
    if (count == nullptr || (list == nullptr && capacity > 0)) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    *count = 0;
    list_state s{list, capacity, 0};
    if (!socketcan_read_links(list_visit, &s)) {
        return ECUIO_ERR_LOAD_FAILED;
    }
    *count = s.count;
    return ECUIO_OK;
}
