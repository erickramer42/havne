// File/layer: tests/fake_ecu.h | test-only, C++17, Qt-free, no platform
//             headers; header-only
// Testing: the HARNESS fakes' one ECU: the fake J2534 DLL
//          (fake_j2534.cpp) and the fake ELM327 (fake_elm327.h) answer
//          every request from here, so the J2534 and ELM327 suites see the
//          same vehicle
//
// What the fakes' engine ECU answers, from the service byte on: the
// adapter's framing (J2534's 4-byte CAN ID, the ELM327's lines and PCI
// bytes) is each fake's own. Its diagnostics (stored, pending and
// permanent DTCs, a freeze frame, readiness) are one state per process
// that Service 04 clears: diagnostics() below.

#ifndef ECUIO_TESTS_FAKE_ECU_H
#define ECUIO_TESTS_FAKE_ECU_H

#include "ecuio/j2534_ptypes.h" /* ECUIO_PT_MSG_DATA_SIZE */

#include <atomic>
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
/* PID 0x7F, engine run time: J1979's 13 data bytes, a reply no Single
 * Frame holds (the support byte, then the total, idle and PTO times, 4
 * bytes each in seconds: 3600, 900 and 0). Supported, but past the
 * bitmaps' chain (0x60 isn't), so the probe drops it: a test asking it
 * turns the probe off. */
constexpr uint8_t kPid7FRunTime[13] = {0x07, 0x00, 0x00, 0x0E, 0x10, 0x00, 0x00,
                                       0x03, 0x84, 0x00, 0x00, 0x00, 0x00};

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

/* ---- Diagnostics (SAE J1979 on ISO 15765-4) ----
 * A DTC is two bytes: the category in A7-A6 (00 P, 01 C, 10 B, 11 U),
 * then the digits. Each list covers different categories, so a decoder
 * sees all four. Three stored DTCs make Service 03's reply, [43][count]
 * [DTCs], 8 bytes: past a Single Frame, so the ISO-TP fakes send it as
 * a First Frame and a Consecutive Frame. */
constexpr uint8_t kStoredDtcs[] = {0x03, 0x01,   /* P0301 */
                                   0x04, 0x20,   /* P0420 */
                                   0xC1, 0x00};  /* U0100 */
constexpr uint8_t kPendingDtcs[] = {0x01, 0x71,  /* P0171 */
                                    0x40, 0x35,  /* C0035 */
                                    0x90, 0x01}; /* B1001 */
/* Service 0x0A's permanent DTCs, which Service 04 leaves: only the ECU
 * clears one, once its monitor passes. J1979 (2002) predates Service
 * 0x0A; its reply takes 03's and 07's shape. */
constexpr uint8_t kPermanentDtcs[] = {0x04, 0x20}; /* P0420 */

/* Service 01 PID 01's bytes B-D (PID 0x41's too): spark ignition (B3
 * clear); misfire, fuel system and components monitored (B2-B0); the
 * catalyst, evaporative system, oxygen sensor, its heater and EGR/VVT
 * (C). Completeness bits are 0 for complete: all complete until a
 * clear, after which C's monitors are incomplete (D = C). */
constexpr uint8_t kMonitorsB = 0x07;
constexpr uint8_t kMonitorsC = 0xE5;

/* What a clear resets to 0, with their values before it: distance with
 * the MIL on (0x21, km), warm-ups (0x30), distance (0x31, km), minutes
 * with the MIL on (0x4D) and since the clear (0x4E). */
constexpr uint8_t kPid21MilKm[2] = {0x00, 0x2A}; /* 42 km */
constexpr uint8_t kPid30WarmUps = 20;
constexpr uint8_t kPid31ClearKm[2] = {0x04, 0xD2};  /* 1234 km */
constexpr uint8_t kPid4DMilMin[2] = {0x00, 0x3C};   /* 60 min */
constexpr uint8_t kPid4EClearMin[2] = {0x0B, 0xB8}; /* 3000 min */

/* The freeze frame (Service 02, frame 0) that P0301 stored: values
 * unlike the live canned ones, so a test can tell the two apart. */
constexpr uint8_t kFreezeDtc[2] = {0x03, 0x01};   /* P0301 */
constexpr uint8_t kFreeze04Load = 0xB3;           /* ~70% */
constexpr uint8_t kFreeze05Coolant = 130;         /* 90 degC */
constexpr uint8_t kFreeze0CRpm[2] = {0x2E, 0xE0}; /* 3000 RPM */
constexpr uint8_t kFreeze0DSpeed = 80;            /* 80 km/h */

/* The diagnostics' state: one per process (the fake J2534 DLL runs in
 * each session's surrogate, so it starts fresh there; the in-process
 * fakes share it across a test binary's cases). cleared: Service 04 ran.
 * clears: how many times. clear_nrc: nonzero refuses Service 04 with
 * that code (J1979: an ECU that can't clear with the engine running
 * answers 0x22, conditionsNotCorrect). engine_off: PID 0C reads 0
 * rpm, the key on and the engine stopped, which a clear asks for
 * (ADR-0038). Tests that clear call reset_diagnostics() first. */
struct Diagnostics {
    std::atomic<bool> cleared{false};
    std::atomic<unsigned> clears{0};
    std::atomic<int> clear_nrc{0};
    std::atomic<bool> engine_off{false};
};

inline Diagnostics& diagnostics()
{
    static Diagnostics d;
    return d;
}

inline void reset_diagnostics()
{
    Diagnostics& d = diagnostics();
    d.cleared = false;
    d.clears = 0;
    d.clear_nrc = 0;
    d.engine_off = false;
}

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
 * bitmaps that lead to them (all but 0x7F's). */
inline bool supports_pid(unsigned pid)
{
    switch (pid) {
    case 0x00:
    case 0x01:
    case 0x04:
    case 0x05:
    case 0x0C:
    case 0x0D:
    case 0x0F:
    case 0x10:
    case 0x11:
    case 0x20:
    case 0x21:
    case 0x2F:
    case 0x30:
    case 0x31:
    case 0x40:
    case 0x41:
    case 0x42:
    case 0x4D:
    case 0x4E:
    case 0x5C:
    case 0x7F:
        return true;
    default:
        return false;
    }
}

/* The Service 02 PIDs frame 0 holds: the DTC that stored it (0x02) and
 * the values above; after a clear, 0x02 alone, reporting 00 00 (J1979:
 * no freeze frame stored). */
inline bool supports_freeze_pid(unsigned pid)
{
    if (diagnostics().cleared) {
        return pid == 0x02;
    }
    return pid == 0x02 || pid == 0x04 || pid == 0x05 || pid == 0x0C ||
           pid == 0x0D;
}

/* A list reply, [service + 0x40][count][DTCs], into out. */
inline void put_dtcs(uint8_t sid_pr, const uint8_t* dtcs, size_t n,
                     unsigned char* out, size_t* out_len)
{
    out[0] = sid_pr;
    out[1] = static_cast<unsigned char>(n / 2);
    if (n > 0) {
        memcpy(out + 2, dtcs, n);
    }
    *out_len = 2 + n;
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
 * [value] for the DIDs above. The diagnostics, from diagnostics()'s
 * state: Services 03, 07 and 0x0A (1 byte) the DTC lists, [43|47|4A]
 * [count][DTCs]; Service 02 ([02][PID][frame], 3 bytes) the freeze
 * frame, [42][PID][frame][data]; Service 04 (1 byte) clears, [44], or
 * refuses with clear_nrc. Any other PID, item, frame or DID gets a
 * negative reply, 7F 01 12 (what the first vehicle answered for PID
 * 0x5C), 7F 02 12, 7F 09 12 or 7F 22 31. unsupported_nrc is the fakes'
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
    Diagnostics& diag = diagnostics();
    const bool cleared = diag.cleared;
    if (req_len == 1) {
        switch (req[0]) {
        case 0x03:
            put_dtcs(0x43, kStoredDtcs, cleared ? 0 : sizeof(kStoredDtcs), out,
                     out_len);
            return true;
        case 0x07:
            put_dtcs(0x47, kPendingDtcs, cleared ? 0 : sizeof(kPendingDtcs),
                     out, out_len);
            return true;
        case 0x0A:
            put_dtcs(0x4A, kPermanentDtcs, sizeof(kPermanentDtcs), out,
                     out_len);
            return true;
        case 0x04: {
            const int nrc = diag.clear_nrc;
            if (nrc != 0) {
                out[0] = 0x7F;
                out[1] = 0x04;
                out[2] = static_cast<unsigned char>(nrc);
                *out_len = 3;
                return true;
            }
            diag.cleared = true;
            diag.clears.fetch_add(1);
            out[0] = 0x44;
            *out_len = 1;
            return true;
        }
        default:
            return false;
        }
    }
    if (req[0] == 0x02 && req_len == 3) {
        const uint8_t pid = req[1];
        if (req[2] != 0x00 || (pid != 0x00 && !supports_freeze_pid(pid))) {
            return refuse(0x12); /* only frame 0 is stored */
        }
        out[0] = 0x42;
        out[1] = pid;
        out[2] = 0x00;
        const auto put = [&](const uint8_t* v, size_t n) {
            memcpy(out + 3, v, n);
            *out_len = 3 + n;
        };
        const uint8_t none[2] = {0x00, 0x00};
        switch (pid) {
        case 0x00: {
            uint8_t bitmap[4] = {};
            for (unsigned k = 1; k <= 32; ++k) {
                if (supports_freeze_pid(k)) {
                    bitmap[(k - 1) / 8] = static_cast<uint8_t>(
                        bitmap[(k - 1) / 8] | (0x80u >> ((k - 1) % 8)));
                }
            }
            put(bitmap, sizeof(bitmap));
            break;
        }
        case 0x02:
            put(cleared ? none : kFreezeDtc, 2);
            break;
        case 0x04:
            put(&kFreeze04Load, 1);
            break;
        case 0x05:
            put(&kFreeze05Coolant, 1);
            break;
        case 0x0C:
            put(kFreeze0CRpm, 2);
            break;
        case 0x0D:
            put(&kFreeze0DSpeed, 1);
            break;
        default:
            return false;
        }
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
    const uint8_t zero[2] = {0x00, 0x00};
    switch (pid) {
    case 0x00:
    case 0x20:
    case 0x40:
        pid_bitmap(pid, out + 2);
        *out_len = 6;
        break;
    case 0x01:
    case 0x41: {
        /* 0x01: A7 the MIL, A6-A0 the stored DTCs' count; 0x41's A is
         * always 0 */
        const uint8_t stored = static_cast<uint8_t>(sizeof(kStoredDtcs) / 2);
        const uint8_t a = (pid == 0x41 || cleared)
                              ? 0x00
                              : static_cast<uint8_t>(0x80 | stored);
        const uint8_t status[4] = {a, kMonitorsB, kMonitorsC,
                                   cleared ? kMonitorsC
                                           : static_cast<uint8_t>(0x00)};
        put(status, sizeof(status));
        break;
    }
    case 0x21:
        put(cleared ? zero : kPid21MilKm, 2);
        break;
    case 0x30: {
        const uint8_t warm_ups = cleared ? 0 : kPid30WarmUps;
        put(&warm_ups, 1);
        break;
    }
    case 0x31:
        put(cleared ? zero : kPid31ClearKm, 2);
        break;
    case 0x4D:
        put(cleared ? zero : kPid4DMilMin, 2);
        break;
    case 0x4E:
        put(cleared ? zero : kPid4EClearMin, 2);
        break;
    case 0x04:
        put(&kPid04EngineLoad, 1);
        break;
    case 0x05:
        put(&kPid05CoolantTemp, 1);
        break;
    case 0x0C:
        put(diag.engine_off ? zero : kPid0CRpm, 2);
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
    case 0x7F:
        put(kPid7FRunTime, sizeof(kPid7FRunTime));
        break;
    default:
        return false;
    }
    return true;
}

} // namespace fake_ecu

#endif /* ECUIO_TESTS_FAKE_ECU_H */
