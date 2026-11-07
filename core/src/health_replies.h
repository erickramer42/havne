// File/layer: core/src/health_replies.h | internal, C++17, Qt-free; each
//             ECU's diagnostics and identification from a recording's
//             replies (ADR-0054's appendix "the health report as built")
// Testing: UNIT: HealthTest (test_core)
//
// A recording holds the replies a session's requests brought, not the
// reports core kept: this rebuilds those reports from whole messages on
// the diagnostic reply IDs, through the parsers the request engine uses,
// so a report from a recording reads as one from the session would. Per
// ECU, the last reply of each kind taken wins. Pure: the caller reads the
// frames and reassembles them.
//
// An ECU is a reply ID with at least one positive reply of a kind the
// report shows (41 01, 41 41, 43, 47, 4A, 42, an identification item); a
// refusal alone makes none, since a listener's recording hears every
// module's. A refusal of 03, 07 or 0A is that list's; any other refusal
// says nothing of which request it answers, and 7F xx 78 (still working)
// is no answer. Nothing is marked asked or complete: a recording holds
// replies, not what was asked.

#ifndef ECUIO_HEALTH_REPLIES_H
#define ECUIO_HEALTH_REPLIES_H

#include "diagnostics.h"
#include "pid_support.h"    /* ecuio_service01_keyed */
#include "request_engine.h" /* ecuio_identity_format, ECUIO_IDENT_DATA_OFFSET */
#include "scrub.h"          /* reply_id */

#include "ecuio/core.h"
#include "ecuio/signals.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

namespace ecuio_health_replies {

class builder
{
public:
    /* obd2 decodes the freeze frame's values (ADR-0037 section 4); NULL
     * leaves them out */
    explicit builder(const ecuio_signal_set_t* obd2) : obd2_(obd2) {}

    /* A whole message on id, its service byte first */
    void take(uint32_t id, bool extended, const uint8_t* data, size_t n)
    {
        if (n < 2 || !ecuio_scrub::reply_id(id, extended)) {
            return;
        }
        const std::vector<uint8_t> msg(data, data + n);
        const uint64_t key = (extended ? (1ull << 32) : 0ull) | id;
        switch (data[0]) {
        case 0x41:
            /* PID 01 and PID 41 at the reply's start, as diagnostics'
             * one-shot asks them; a pack that carries one behind another
             * PID isn't read */
            if (n >= 6 && (data[1] == 0x01 || data[1] == 0x41)) {
                ecu& e = ecu_of(key, id);
                e.positive(data[1] == 0x01 ? &e.readiness : &e.cycle, msg);
            }
            break;
        case 0x43:
        case 0x47:
        case 0x4A: {
            ecu& e = ecu_of(key, id);
            e.positive(&e.lists[list_of(static_cast<uint8_t>(data[0] - 0x40))],
                       msg);
            break;
        }
        case 0x42:
            take_freeze(ecu_of(key, id), msg);
            break;
        case 0x49:
            /* An item, not a bitmap of them (00, 20, ...) */
            if (n >= 3 && data[1] % 0x20 != 0) {
                ecu& e = ecu_of(key, id);
                e.positive(&e.items[(0x09u << 16) | (data[1] << 8)], msg);
            }
            break;
        case 0x62:
            if (n >= 3 && data[1] == 0xF1 && data[2] >= 0x80) {
                ecu& e = ecu_of(key, id);
                e.positive(&e.items[(0x22u << 16) | (0xF1u << 8) | data[2]],
                           msg);
            }
            break;
        case 0x7F:
            if (n >= 3 && data[2] != 0x78 &&
                (data[1] == 0x03 || data[1] == 0x07 || data[1] == 0x0A)) {
                last& l = ecu_of(key, id).lists[list_of(data[1])];
                l.seen = true;
                l.refused = true;
                l.nrc = data[2];
                l.msg.clear();
            }
            break;
        default:
            break;
        }
    }

    /* The ECUs found, 11-bit reply IDs first, each by ID */
    void finish(std::vector<ecuio_channel_diagnostics_t>& diagnostics,
                std::vector<ecuio_channel_identity_t>& identity) const
    {
        for (const auto& kv : ecus_) {
            const ecu& e = kv.second;
            if (!e.any_positive) {
                continue;
            }
            diagnostics.push_back(diagnostics_of(e));
            identity.push_back(identity_of(e));
        }
    }

private:
    struct last {
        bool seen = false;
        bool refused = false;
        uint8_t nrc = 0;
        std::vector<uint8_t> msg;
    };

    struct ecu {
        uint32_t id = 0;
        bool any_positive = false;
        last readiness;
        last cycle;
        last lists[ECUIO_DIAG_LISTS];
        last freeze_pids;                                      /* 42 00 00 */
        last freeze_code;                                      /* 42 02 00 */
        std::map<uint8_t, std::vector<uint8_t>> freeze_values; /* by PID */
        std::map<uint32_t, last> items;                        /* by request */

        void positive(last* l, const std::vector<uint8_t>& msg)
        {
            any_positive = true;
            l->seen = true;
            l->refused = false;
            l->nrc = 0;
            l->msg = msg;
        }
    };

    static uint32_t list_of(uint8_t service)
    {
        return service == 0x03   ? ECUIO_DIAG_STORED
               : service == 0x07 ? ECUIO_DIAG_PENDING
                                 : ECUIO_DIAG_PERMANENT;
    }

    ecu& ecu_of(uint64_t key, uint32_t id)
    {
        ecu& e = ecus_[key];
        e.id = id;
        return e;
    }

    /* 42 <pid> <frame> <data>: the freeze frame's PIDs, its code, or a
     * value; frame 0 only, the one the report reads */
    static void take_freeze(ecu& e, const std::vector<uint8_t>& msg)
    {
        if (msg.size() < 3 || msg[2] != 0x00) {
            return;
        }
        if (msg[1] == 0x00) {
            if (msg.size() >= 7) {
                e.positive(&e.freeze_pids, msg);
            }
        } else if (msg[1] == 0x02) {
            if (msg.size() >= 5) {
                e.positive(&e.freeze_code, msg);
            }
        } else if (msg.size() >= 4) {
            e.any_positive = true;
            e.freeze_values[msg[1]] = msg;
        }
    }

    /* A freeze-frame value, 42 <pid> 00 <data>, read as the Service 01
     * reply 41 <pid> <data> it mirrors, through each obd2 def of the PID,
     * as ecuio_engine_diag_take_value reads one */
    void take_value(ecuio_channel_diagnostics_t& d,
                    const std::vector<uint8_t>& msg) const
    {
        if (obd2_ == nullptr || obd2_->defs == nullptr) {
            return;
        }
        uint8_t payload[16] = {0x41, msg[1]};
        uint16_t n = 2;
        for (size_t i = 3; i < msg.size() && n < sizeof payload; ++i) {
            payload[n++] = msg[i];
        }
        for (uint32_t i = 0;
             i < obd2_->count && d.freeze_count < ECUIO_DIAG_MAX_FREEZE; ++i) {
            const ecuio_signal_def_t& def = obd2_->defs[i];
            double v = 0;
            if (ecuio_service01_keyed(def) && (def.mux_key & 0xFFu) == msg[1] &&
                ecuio_decode_signal(&def, payload, n, &v) == 0) {
                ecuio_freeze_value_t& f = d.freeze_values[d.freeze_count++];
                f = ecuio_freeze_value_t{};
                f.def_id = def.id;
                f.value = v;
            }
        }
    }

    ecuio_channel_diagnostics_t diagnostics_of(const ecu& e) const
    {
        ecuio_channel_diagnostics_t d{};
        d.rx_id = e.id;
        d.generation = 1;
        if (e.readiness.seen) {
            d.readiness = ECUIO_DIAG_ANSWERED;
            ecuio_diag_take_readiness(e.readiness.msg.data() + 2, &d);
        }
        if (e.cycle.seen) {
            d.cycle = ECUIO_DIAG_ANSWERED;
            ecuio_diag_take_cycle(e.cycle.msg.data() + 2, &d);
        }
        for (uint32_t l = 0; l < ECUIO_DIAG_LISTS; ++l) {
            const last& src = e.lists[l];
            if (!src.seen) {
                continue;
            }
            d.lists[l].outcome =
                src.refused ? ECUIO_DIAG_REFUSED : ECUIO_DIAG_ANSWERED;
            d.lists[l].nrc = src.nrc;
            if (!src.refused) {
                ecuio_diag_take_codes(src.msg.data(),
                                      static_cast<uint32_t>(src.msg.size()),
                                      &d.lists[l]);
            }
        }
        if (e.freeze_pids.seen || e.freeze_code.seen ||
            !e.freeze_values.empty()) {
            d.freeze = ECUIO_DIAG_ANSWERED;
        }
        if (e.freeze_code.seen) {
            d.freeze_dtc = static_cast<uint16_t>((e.freeze_code.msg[3] << 8) |
                                                 e.freeze_code.msg[4]);
            if (d.freeze_dtc != 0) {
                d.flags |= ECUIO_DIAG_FREEZE_FRAME;
                for (const auto& kv : e.freeze_values) {
                    take_value(d, kv.second);
                }
            }
        }
        return d;
    }

    static ecuio_channel_identity_t identity_of(const ecu& e)
    {
        ecuio_channel_identity_t id{};
        id.rx_id = e.id;
        for (const auto& kv : e.items) {
            if (id.items >= ECUIO_IDENTITY_MAX_ITEMS) {
                break;
            }
            ecuio_identity_item_t& it = id.list[id.items++];
            it.request[0] = static_cast<uint8_t>(kv.first >> 16);
            it.request[1] = static_cast<uint8_t>(kv.first >> 8);
            it.request[2] = static_cast<uint8_t>(kv.first);
            it.length = it.request[0] == 0x09 ? 2 : 3;
            it.outcome = ECUIO_IDENTITY_ANSWERED;
            const std::vector<uint8_t>& msg = kv.second.msg;
            if (msg.size() > ECUIO_IDENT_DATA_OFFSET) {
                it.text_length = ecuio_identity_format(
                    msg.data() + ECUIO_IDENT_DATA_OFFSET,
                    static_cast<uint32_t>(msg.size() - ECUIO_IDENT_DATA_OFFSET),
                    it.text, sizeof it.text);
            }
        }
        if (id.items > 0) {
            id.generation = 1;
        }
        return id;
    }

    const ecuio_signal_set_t* obd2_;
    std::map<uint64_t, ecu> ecus_;
};

} // namespace ecuio_health_replies

#endif /* ECUIO_HEALTH_REPLIES_H */
