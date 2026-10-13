// File/layer: tests/fake_j2534.cpp | Fake J2534 vendor DLL, HARNESS tier
// Testing: exercised via LoadLibraryEx by test_j2534_load/_stream;
//          PASSTHRU_MSG layout shared with core via j2534_ptypes.h
// NOTE: InjectFrame/ReadMsgs share a mutex-guarded FIFO — injections
//      from the test thread race safely against the polling worker.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "qttune/j2534_ptypes.h"

#include <cstring>
#include <mutex>

#define FAKE_QUEUE_DEPTH 16
#define STATUS_NOERROR QTTUNE_PT_STATUS_NOERROR
#define STATUS_FAILED  QTTUNE_PT_STATUS_FAILED

namespace fake_j2534 {

struct FrameSlot {
    unsigned char data[64];
    unsigned long size;
    unsigned long timestamp_us;
};

std::mutex g_lock;
int g_handle = 0;                       /* 1 between Open and Close */
FrameSlot g_queue[FAKE_QUEUE_DEPTH];
unsigned long g_queue_len = 0;

} // namespace fake_j2534

extern "C" __declspec(dllexport)
long __stdcall PassThruOpen(void* pName, unsigned long* pDeviceID)
{
    (void)pName;
    if (pDeviceID == nullptr) {
        return STATUS_FAILED;
    }
    std::lock_guard<std::mutex> lk(fake_j2534::g_lock);
    fake_j2534::g_handle = 1;
    *pDeviceID = 1;
    return STATUS_NOERROR;
}

extern "C" __declspec(dllexport)
long __stdcall PassThruClose(unsigned long DeviceID)
{
    (void)DeviceID;
    std::lock_guard<std::mutex> lk(fake_j2534::g_lock);
    fake_j2534::g_handle = 0;
    return STATUS_NOERROR;
}

extern "C" __declspec(dllexport)
long __stdcall PassThruConnect(unsigned long DeviceID,
                               unsigned long ProtocolID,
                               unsigned long Flags,
                               unsigned long BaudRate,
                               unsigned long* pChannelID)
{
    (void)DeviceID; (void)ProtocolID; (void)Flags; (void)BaudRate;
    std::lock_guard<std::mutex> lk(fake_j2534::g_lock);
    if (fake_j2534::g_handle != 1 || pChannelID == nullptr) {
        return STATUS_FAILED;
    }
    *pChannelID = 1;
    return STATUS_NOERROR;
}

extern "C" __declspec(dllexport)
long __stdcall PassThruDisconnect(unsigned long ChannelID)
{
    (void)ChannelID;
    return STATUS_NOERROR;
}

extern "C" __declspec(dllexport)
long __stdcall PassThruReadMsgs(unsigned long ChannelID,
                                PASSTHRU_MSG* pMsg,
                                unsigned long* pNumMsgs,
                                unsigned long Timeout)
{
    (void)ChannelID; (void)Timeout;
    if (pMsg == nullptr || pNumMsgs == nullptr) {
        return STATUS_FAILED;
    }

    std::lock_guard<std::mutex> lk(fake_j2534::g_lock);
    if (fake_j2534::g_queue_len == 0) {
        *pNumMsgs = 0;   /* quiet bus — idle, not an error */
        return STATUS_NOERROR;
    }

    const fake_j2534::FrameSlot& slot = fake_j2534::g_queue[0];
    memcpy(pMsg->Data, slot.data, slot.size);
    pMsg->DataSize      = slot.size;
    pMsg->ProtocolID    = QTTUNE_PT_PROTOCOL_ISO15765;
    pMsg->RxStatus      = 0;
    pMsg->Timestamp     = slot.timestamp_us;
    pMsg->ExtraDataIndex = 0;

    /* FIFO shift — depth 16 makes O(n) irrelevant */
    for (unsigned long i = 1; i < fake_j2534::g_queue_len; ++i) {
        fake_j2534::g_queue[i - 1] = fake_j2534::g_queue[i];
    }
    fake_j2534::g_queue_len--;

    *pNumMsgs = 1;
    return STATUS_NOERROR;
}

extern "C" __declspec(dllexport)
long __stdcall PassThruWriteMsgs(unsigned long ChannelID,
                                 PASSTHRU_MSG* pMsg,
                                 unsigned long* pNumMsgs,
                                 unsigned long Timeout)
{
    (void)ChannelID; (void)pMsg; (void)pNumMsgs; (void)Timeout;
    return STATUS_NOERROR;   /* echo-accept: consume and discard */
}

extern "C" __declspec(dllexport)
long __stdcall PassThruIoCtl(unsigned long ChannelID,
                             long IoControlID,
                             void* pInput,
                             void* pOutput)
{
    (void)ChannelID; (void)IoControlID; (void)pInput; (void)pOutput;
    return STATUS_FAILED;   /* not supported by the fake */
}

/* ================= Test hooks (TEST_HOOKS_ENABLED only) ============== */
#if defined(TEST_HOOKS_ENABLED) || defined(_DEBUG)

extern "C" __declspec(dllexport)
void InjectFrame(const unsigned char* data, unsigned long size,
                 unsigned long timestamp_us,
                 unsigned long is_extended,
                 unsigned long channel_id)
{
    (void)is_extended; (void)channel_id;
    if (data == nullptr || size == 0 || size > 64) {
        return;
    }

    std::lock_guard<std::mutex> lk(fake_j2534::g_lock);
    if (fake_j2534::g_queue_len >= FAKE_QUEUE_DEPTH) {
        return;   /* queue full — documented drop, keeps tests honest */
    }
    fake_j2534::FrameSlot& slot =
        fake_j2534::g_queue[fake_j2534::g_queue_len++];
    memcpy(slot.data, data, size);
    slot.size = size;
    slot.timestamp_us = timestamp_us;
}

extern "C" __declspec(dllexport)
void GetBufferState(unsigned char** out_buffer, unsigned long* out_size)
{
    if (out_buffer == nullptr || out_size == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lk(fake_j2534::g_lock);
    if (fake_j2534::g_queue_len > 0) {
        *out_buffer = fake_j2534::g_queue[0].data;
        *out_size   = fake_j2534::g_queue[0].size;
    } else {
        *out_buffer = nullptr;
        *out_size = 0;
    }
}

extern "C" __declspec(dllexport)
int GetCurrentHandle(void)
{
    std::lock_guard<std::mutex> lk(fake_j2534::g_lock);
    return fake_j2534::g_handle;
}

#endif // TEST_HOOKS_ENABLED || _DEBUG
