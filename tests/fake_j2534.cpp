// File/layer: tests/fake_j2534.cpp | Fake J2534 vendor DLL, HARNESS tier
// Testing: Fake DLL artifact — unit-tested via LoadLibraryEx in j2534_load tests

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdint>
#include <cstring>

/* =================================================================
 * SAE J2534-1 v4.04 Type Definitions (minimal subset for fake impl)
 * Uses SDK types where available, avoids collisions.
 * ================================================================= */

/* Protocol constants (subset, enough for canned response) */
#define PASSTHRU_ISO15765   0x00000100
#define PASSTHRU_ISO9141    0x00000020
#define PASSTHRU_J1850VPW   0x00000010
#define PASSTHRU_J1850SPWM  0x00000040
#define PASSTHRU_CAN        0x00000080

/* Message structure (matches real J2534 spec layout) */
struct PASSTHRU_MSG {
    DWORD RxStatus;
    DWORD TxFlags;
    DWORD timestamp;
    DWORD statusBlock;
    DWORD extraDataIndex;
    unsigned char Data[256];
    unsigned char ExtraData[1024];
    DWORD Size;
    DWORD NetworkID;
};

/* Error codes (subset) */
#define STATUS_NOERROR          0
#define STATUS_NOTUSED          1
#define STATUS_FAILED           0xC0000000L
#define STATUS_BUFFER_OVERFLOW  0xC0000001L
#define STATUS_NOT_SUPPORTED    0xC0000002L

/* =================================================================
 * Global state (test hook access)
 * ================================================================= */
namespace fake_j2534 {
    static int g_handle = 0;      // Track if PassThruOpen was called
    static unsigned char g_buffer[256];  // Buffer for injection
    static size_t g_buffer_size = 0;
}

/* =================================================================
 * J2534 API Implementation (minimal for testing)
 * ================================================================= */

extern "C" __declspec(dllexport)
long __stdcall PassThruOpen(long* pErrCode)
{
    if (pErrCode == nullptr) {
        return STATUS_FAILED;
    }
    
    fake_j2534::g_handle = 1;
    *pErrCode = STATUS_NOERROR;
    return 1;  // Handle ID (must be non-zero for success)
}

extern "C" __declspec(dllexport)
long __stdcall PassThruClose(long hDevice)
{
    (void)hDevice;
    fake_j2534::g_handle = 0;
    return STATUS_NOERROR;
}

extern "C" __declspec(dllexport)
long __stdcall PassThruConnect(long hDevice,
                               unsigned long Protocol,
                               unsigned long Flags,
                               unsigned long Baud,
                               unsigned long TxFlags,
                               unsigned long* pChannel)
{
    if (fake_j2534::g_handle != 1 || pChannel == nullptr) {
        return STATUS_FAILED;
    }
    
    /* Accept any protocol (we're not validating protocol behavior) */
    *pChannel = 1;  // Fixed channel ID
    return STATUS_NOERROR;
}

extern "C" __declspec(dllexport)
long __stdcall PassThruDisconnect(long hChannel)
{
    (void)hChannel;
    /* Disconnect is idempotent, doesn't affect global state */
    return STATUS_NOERROR;
}

extern "C" __declspec(dllexport)
long __stdcall PassThruReadMsgs(long hChannel,
                                PASSTHRU_MSG* pMsg,
                                unsigned long* pNumMsgs,
                                unsigned long Timeout)
{
    (void)hChannel;
    (void)pNumMsgs;
    (void)Timeout;
    
    /* Default: timeout without data (fake device with no traffic) */
    pMsg->RxStatus = 0;
    pMsg->Size = 0;
    pMsg->timestamp = 0;
    
    return STATUS_NOERROR;
}

extern "C" __declspec(dllexport)
long __stdcall PassThruWriteMsgs(long hChannel,
                                 PASSTHRU_MSG* pMsg,
                                 unsigned long* pNumMsgs,
                                 unsigned long Timeout)
{
    (void)hChannel;
    (void)pNumMsgs;
    (void)Timeout;
    return STATUS_NOERROR;  // Echo-back mode: accept and discard
}

extern "C" __declspec(dllexport)
long __stdcall PassThruIoCtl(long hChannel,
                             long IoControlID,
                             void* pInput,
                             void* pOutput)
{
    (void)hChannel;
    (void)IoControlID;
    (void)pInput;
    (void)pOutput;
    return STATUS_NOT_SUPPORTED;
}

/* =================================================================
 * Test Hook (exported only in DEBUG/Test builds)
 * ================================================================= */
#if defined(TEST_HOOKS_ENABLED) || defined(_DEBUG)

extern "C" __declspec(dllexport)
void InjectFrame(const unsigned char* data, unsigned long size,
                 unsigned long timestamp_us,
                 unsigned long is_extended,
                 unsigned long channel_id)
{
    if (size > sizeof(fake_j2534::g_buffer)) {
        return;
    }
    
    /* Copy frame data to internal buffer */
    memcpy(fake_j2534::g_buffer, data, size);
    fake_j2534::g_buffer_size = size;
    
    /* Mark as ready for next ReadMsgs call */
    /* Note: in a full impl, this would queue the frame for async delivery */
    /* For fake DLL, the next ReadMsgs polls this buffer */
}

extern "C" __declspec(dllexport)
void GetBufferState(unsigned char** out_buffer, unsigned long* out_size)
{
    *out_buffer = fake_j2534::g_buffer;
    *out_size = static_cast<unsigned long>(fake_j2534::g_buffer_size);
}

extern "C" __declspec(dllexport)
int GetCurrentHandle(void)
{
    return fake_j2534::g_handle;
}

#endif // TEST_HOOKS_ENABLED || _DEBUG
