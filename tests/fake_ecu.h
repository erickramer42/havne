// File/layer: tests/fake_ecu.h | test-only, C++17, Qt-free, no platform
//             headers; header-only
// Testing: the HARNESS fakes' one ECU: the fake J2534 DLL
//          (fake_j2534.cpp) and the fake ELM327 (fake_elm327.h) answer
//          every request from here, so the J2534 and ELM327 suites see the
//          same vehicle
//
// What the fakes' engine ECU answers, from the service byte on: the
// adapter's framing (J2534's 4-byte CAN ID, the ELM327's lines and PCI
// bytes) is each fake's own.

#ifndef ECUIO_TESTS_FAKE_ECU_H
#define ECUIO_TESTS_FAKE_ECU_H

#include "ecuio/j2534_ptypes.h" /* ECUIO_PT_MSG_DATA_SIZE */

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace fake_ecu {

/* ---- OBD-II canned response data (ADR-0013) ----
 * Standard PIDs that express as simple byte-extraction + scale/offset.
 * Each value is a plausible reading on its own, not one consistent
 * engine state: RPM 2000, speed 0, load ~10%, throttle ~8%, coolant
 * and IAT 20 degC, oil 80 degC, fuel ~25%, voltage ~13.9 V, MAF
 * 1.00 g/s. Oil reads warmer than coolant and IAT so a test can tell
 * the A - 40 temperatures apart. */
constexpr uint8_t kPid04EngineLoad = 25;         /* 25 * 100/255 ~= 9.8% */
constexpr uint8_t kPid05CoolantTemp = 60;        /* 60 - 40 = 20 degC */
constexpr uint8_t kPid0CRpm[2] = {0x1F, 0x40};   /* 0x1F40 * 0.25 = 2000 RPM */
constexpr uint8_t kPid0DSpeed = 0;               /* 0 km/h */
constexpr uint8_t kPid0FIat = 60;                /* 20 degC */
constexpr uint8_t kPid10Maf[2] = {0x00, 0x64};   /* 0x0064 * 0.01 = 1.00 g/s */
constexpr uint8_t kPid11Throttle = 20;           /* ~= 7.8% */
constexpr uint8_t kPid2FFuelLevel = 64;          /* ~= 25% tank */
constexpr uint8_t kPid42Volts[2] = {0x36, 0x30}; /* 0x3630 = 13872 mV */
constexpr uint8_t kPid5COilTemp = 0x78;          /* 120 - 40 = 80 degC */

/* The Service 0x22 DIDs the ECU knows: ADR-0015's example, and three
 * whose replies are long, filled with their byte index (& 0xFF). A
 * reply is [62][DID][data], so 0xF1A1's 4092 bytes make the longest
 * ISO 15765-2 message on classic CAN, 4095, and 0xF1A2's fill
 * PASSTHRU_MSG.Data behind the 4-byte CAN ID, past what a frame holds
 * (and past what ISO-TP carries, so only the J2534 fake sends it). */
constexpr uint16_t kDid = 0x1E1C;
constexpr unsigned char kDidValue[2] = {0x12, 0x34};
constexpr uint16_t kLongDid = 0xF1A0;
constexpr unsigned long kLongDidLen = 200;
constexpr uint16_t kMaxDid = 0xF1A1;
constexpr unsigned long kMaxDidLen = 4092;
constexpr uint16_t kOverlongDid = 0xF1A2;
constexpr unsigned long kOverlongDidLen = ECUIO_PT_MSG_DATA_SIZE - 7;

/* What the ECU says it is (ADR-0021), every value marked FAKE so none
 * can pass for a vehicle's. The VIN has a VIN's 17 characters; the
 * CALID and the ECU name are NUL-padded to J1979's 16 and 20 bytes; the
 * CVN is four binary bytes. */
constexpr char kVin[] = "FAKEJ2534VIN00001";
constexpr char kCalid[16] = "FAKECAL-01";
constexpr unsigned char kCvn[4] = {0xDE, 0xAD, 0xBE, 0xEF};
constexpr char kEcuName[20] = "FAKE-ECM";

/* The longest reply answer() writes: [62][DID] and 0xF1A2's data. */
constexpr size_t kReplyMax = 3 + kOverlongDidLen;

/* The ID an ECU answers a request on: 11-bit physical requests 0x7E0 to
 * 0x7E7 get theirs 8 above, the functional 0x7DF is answered by the
 * engine ECU (0x7E8), and a 29-bit 18DA<ecu><tester> request by
 * 18DA<tester><ecu> (ISO 15765-4). 0 for any other ID. */
inline uint32_t reply_id(uint32_t tx)
{
    if (tx == 0x7DFu) {
        return 0x7E8u;
    }
    if (tx >= 0x7E0u && tx <= 0x7E7u) {
        return tx + 8;
    }
    if ((tx & 0xFFFF0000u) == 0x18DA0000u) {
        return 0x18DA0000u | ((tx & 0xFFu) << 8) | ((tx >> 8) & 0xFFu);
    }
    return 0;
}

/* The identification DIDs the ECU answers, as text; nullptr for any
 * other DID. 0xF194 and 0xF195 are left out, so identification meets a
 * refusal. */
inline const char* did_text(uint16_t did)
{
    switch (did) {
    case 0xF187:
        return "FAKE-PART-0001";
    case 0xF188:
        return "FAKE-SW-0001";
    case 0xF189:
        return "FAKE 1.0";
    case 0xF18A:
        return "FAKE SUPPLIER";
    case 0xF18C:
        return "FAKE-SERIAL-000001";
    case 0xF190:
        return kVin;
    case 0xF191:
        return "FAKE-HW-01";
    default:
        return nullptr;
    }
}

/* A DID's data length, or 0 for one the ECU doesn't know. */
inline unsigned long did_length(uint16_t did)
{
    if (const char* text = did_text(did)) {
        return static_cast<unsigned long>(strlen(text));
    }
    switch (did) {
    case kDid:
        return sizeof(kDidValue);
    case kLongDid:
        return kLongDidLen;
    case kMaxDid:
        return kMaxDidLen;
    case kOverlongDid:
        return kOverlongDidLen;
    default:
        return 0;
    }
}

/* The Service 01 PIDs the ECU supports: its canned ones, and the
 * bitmaps that lead to them. */
inline bool supports_pid(unsigned pid)
{
    switch (pid) {
    case 0x00:
    case 0x04:
    case 0x05:
    case 0x0C:
    case 0x0D:
    case 0x0F:
    case 0x10:
    case 0x11:
    case 0x20:
    case 0x2F:
    case 0x40:
    case 0x42:
    case 0x5C:
        return true;
    default:
        return false;
    }
}

/* The supported-PID bitmap at PID base (0x00, 0x20, 0x40), into out[4]:
 * PID base + k (k from 1 to 32) is bit 0x80 >> ((k - 1) % 8) of byte
 * (k - 1) / 8 (SAE J1979), the last bit the next bitmap. */
inline void pid_bitmap(unsigned base, unsigned char* out)
{
    out[0] = out[1] = out[2] = out[3] = 0;
    for (unsigned k = 1; k <= 32; ++k) {
        const unsigned char bit =
            static_cast<unsigned char>(0x80u >> ((k - 1) % 8));
        if (supports_pid(base + k)) {
            out[(k - 1) / 8] =
                static_cast<unsigned char>(out[(k - 1) / 8] | bit);
        }
    }
}

/* The ECU's answer to a Service 01 or 09 request (req is [service][PID
 * or item], 2 bytes) or a Service 0x22 one ([22][DID], 3 bytes), into
 * out (kReplyMax bytes), from the service byte on: [41][PID][canned]
 * from the values above or a bitmap; [49][item][01][data] for Service
 * 09's VIN (02), CALID (04), CVN (06) and ECU name (0A); or [62][DID]
 * [value] for the DIDs above. Any other PID, item or DID gets a
 * negative reply, 7F 01 12 (what the first vehicle answered for PID
 * 0x5C), 7F 09 12 or 7F 22 31. unsupported_nrc is the fakes'
 * ECUIO_FAKE_UNSUPPORTED_NRC: -1 (unset) keeps those codes, 0 answers
 * nothing (an ECU that stays silent), any other value is the code for
 * all three. False when nothing answers. */
inline bool answer(const unsigned char* req, size_t req_len,
                   long unsupported_nrc, unsigned char* out, size_t* out_len)
{
    const auto refuse = [&](unsigned char fallback) {
        if (unsupported_nrc == 0) {
            return false;
        }
        out[0] = 0x7F;
        out[1] = req[0];
        out[2] = (unsupported_nrc < 0)
                     ? fallback
                     : static_cast<unsigned char>(unsupported_nrc);
        *out_len = 3;
        return true;
    };
    if (req[0] == 0x22 && req_len == 3) {
        const uint16_t did = static_cast<uint16_t>((req[1] << 8) | req[2]);
        const unsigned long n = did_length(did);
        if (n == 0) {
            return refuse(0x31); /* request out of range */
        }
        out[0] = 0x62;
        out[1] = req[1];
        out[2] = req[2];
        const char* text = did_text(did);
        for (unsigned long i = 0; i < n; ++i) {
            out[3 + i] = (text != nullptr) ? static_cast<unsigned char>(text[i])
                         : (did == kDid) ? kDidValue[i]
                                         : static_cast<unsigned char>(i & 0xFF);
        }
        *out_len = 3 + n;
        return true;
    }
    if (req_len != 2) {
        return false;
    }
    if (req[0] == 0x09) {
        /* Vehicle information: [49][item][count 01][data]. */
        const unsigned char* data = nullptr;
        size_t n = 0;
        switch (req[1]) {
        case 0x02:
            data = reinterpret_cast<const unsigned char*>(kVin);
            n = sizeof(kVin) - 1;
            break;
        case 0x04:
            data = reinterpret_cast<const unsigned char*>(kCalid);
            n = sizeof(kCalid);
            break;
        case 0x06:
            data = kCvn;
            n = sizeof(kCvn);
            break;
        case 0x0A:
            data = reinterpret_cast<const unsigned char*>(kEcuName);
            n = sizeof(kEcuName);
            break;
        default:
            return refuse(0x12);
        }
        out[0] = 0x49;
        out[1] = req[1];
        out[2] = 0x01;
        memcpy(out + 3, data, n);
        *out_len = 3 + n;
        return true;
    }
    if (req[0] != 0x01) {
        return false;
    }
    const uint8_t pid = req[1];
    if (!supports_pid(pid)) {
        return refuse(0x12); /* the first vehicle's answer for 0x5C */
    }
    out[0] = 0x41; /* Mode 01 + 0x40 response */
    out[1] = pid;  /* PID echo */
    const auto put = [&](const uint8_t* v, size_t n) {
        memcpy(out + 2, v, n);
        *out_len = 2 + n;
    };
    switch (pid) {
    case 0x00:
    case 0x20:
    case 0x40:
        pid_bitmap(pid, out + 2);
        *out_len = 6;
        break;
    case 0x04:
        put(&kPid04EngineLoad, 1);
        break;
    case 0x05:
        put(&kPid05CoolantTemp, 1);
        break;
    case 0x0C:
        put(kPid0CRpm, 2);
        break;
    case 0x0D:
        put(&kPid0DSpeed, 1);
        break;
    case 0x0F:
        put(&kPid0FIat, 1);
        break;
    case 0x10:
        put(kPid10Maf, 2);
        break;
    case 0x11:
        put(&kPid11Throttle, 1);
        break;
    case 0x2F:
        put(&kPid2FFuelLevel, 1);
        break;
    case 0x42:
        put(kPid42Volts, 2);
        break;
    case 0x5C:
        put(&kPid5COilTemp, 1);
        break;
    default:
        return false;
    }
    return true;
}

} // namespace fake_ecu

#endif /* ECUIO_TESTS_FAKE_ECU_H */
