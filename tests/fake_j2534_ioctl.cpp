// File/layer: tests/fake_j2534_ioctl.cpp | Fake J2534 vendor DLL, HARNESS tier;
//             built in BOTH trees
// Testing: PassThruIoctl of the fake (see fake_j2534.cpp)

#include "fake_j2534_state.h"

#include <atomic>

extern "C"
long __stdcall PassThruIoctl(unsigned long ChannelID,
                             long IoControlID,
                             void* pInput,
                             void* pOutput)
{
    (void)ChannelID;

    /* One-shot env-armed stall. The flag is taken under the lock and
     * the sleep outside it, so other callers of the fake (which share
     * the mutex) are never blocked by the stall. */
    bool do_stall = false;
    char stall_buf[32] = {};
    {
        if (fake_j2534::read_knob("ECUIO_FAKE_IOCTL_DELAY_MS",
                                  stall_buf, sizeof(stall_buf))) {
            std::lock_guard<std::mutex> lk(fake_j2534::g_lock);
            if (!fake_j2534::g_ioctl_delay_fired) {
                fake_j2534::g_ioctl_delay_fired = true;
                do_stall = true;
            }
        }
    }
    if (do_stall) {
        const DWORD delay =
            static_cast<DWORD>(strtoul(stall_buf, nullptr, 10));
        if (delay > 0 && delay <= 60000) Sleep(delay);
    }
    unsigned long vbatt_mv = 13800;
    if (IoControlID == static_cast<long>(ECUIO_PT_IOCTL_READ_VBATT)) {
        if (const long rc = fake_j2534::refusal("ECUIO_FAKE_VBATT_STATUS",
                                                "READ_VBATT")) {
            return rc;
        }
        static std::atomic<unsigned long> reads{0};
        const unsigned long nth = ++reads;
        char nb[32] = {};
        if (fake_j2534::read_knob("ECUIO_FAKE_VBATT", nb, sizeof(nb))) {
            vbatt_mv = strtoul(nb, nullptr, 10);
        }
        /* <mv>:<n>: from the nth READ_VBATT since load on, the connect's
         * own the first */
        if (fake_j2534::read_knob("ECUIO_FAKE_VBATT_SAG", nb, sizeof(nb))) {
            char* at = nullptr;
            const unsigned long sag = strtoul(nb, &at, 10);
            if (*at == ':' && nth >= strtoul(at + 1, nullptr, 10)) {
                vbatt_mv = sag;
            }
        }
    }

    std::lock_guard<std::mutex> lk(fake_j2534::g_lock);

    switch (IoControlID) {
    case ECUIO_PT_IOCTL_GET_CONFIG: {
        if (pInput == nullptr) {
            return STATUS_FAILED;
        }
        SCONFIG_LIST* list = static_cast<SCONFIG_LIST*>(pInput);
        if (list == nullptr || list->ConfigPtr == nullptr ||
            list->NumOfParams == 0 ||
            list->NumOfParams > FAKE_CONFIG_DEPTH) {
            return STATUS_FAILED;
        }
        for (unsigned long i = 0; i < list->NumOfParams; ++i) {
            unsigned long value = 0xFFFFFFFF;   /* "vendor default" */
            for (unsigned long j = 0; j < fake_j2534::g_cfg_len; ++j) {
                if (fake_j2534::g_cfg_param[j] ==
                    list->ConfigPtr[i].Parameter) {
                    value = fake_j2534::g_cfg_value[j];
                    break;
                }
            }
            list->ConfigPtr[i].Value = value;
        }
        return STATUS_NOERROR;
    }
    case ECUIO_PT_IOCTL_SET_CONFIG: {
        if (pInput == nullptr) {
            return STATUS_FAILED;
        }
        SCONFIG_LIST* list = static_cast<SCONFIG_LIST*>(pInput);
        if (list == nullptr || list->ConfigPtr == nullptr ||
            list->NumOfParams == 0 ||
            list->NumOfParams > FAKE_CONFIG_DEPTH) {
            return STATUS_FAILED;
        }
        for (unsigned long i = 0; i < list->NumOfParams; ++i) {
            bool stored = false;
            for (unsigned long j = 0; j < fake_j2534::g_cfg_len; ++j) {
                if (fake_j2534::g_cfg_param[j] ==
                    list->ConfigPtr[i].Parameter) {
                    fake_j2534::g_cfg_value[j] = list->ConfigPtr[i].Value;
                    stored = true;
                    break;
                }
            }
            if (!stored && fake_j2534::g_cfg_len < FAKE_CONFIG_DEPTH) {
                fake_j2534::g_cfg_param[fake_j2534::g_cfg_len] =
                    list->ConfigPtr[i].Parameter;
                fake_j2534::g_cfg_value[fake_j2534::g_cfg_len] =
                    list->ConfigPtr[i].Value;
                fake_j2534::g_cfg_len++;
            }
        }
        return STATUS_NOERROR;
    }
    case ECUIO_PT_IOCTL_READ_VBATT:
        /* Contract pin: v04.04 passes the DeviceID
         * to READ_VBATT. This fake's PassThruOpen returns device 1 and
         * PassThruConnect returns channel 0x55: a caller passing the
         * channel FAILS here instead of passing by coincidence. */
        if (ChannelID != fake_j2534::g_device_id) {
            return STATUS_FAILED;
        }
        if (pOutput != nullptr) {
            *static_cast<unsigned long*>(pOutput) = vbatt_mv;
        }
        return STATUS_NOERROR;
    case ECUIO_PT_IOCTL_CLEAR_TX_BUFFER:
    case ECUIO_PT_IOCTL_CLEAR_RX_BUFFER:
    case ECUIO_PT_IOCTL_CLEAR_MSG_FILTERS:
        return STATUS_NOERROR;
    default:
        return STATUS_FAILED;   /* strict vendor: unknown = failure */
    }
}
