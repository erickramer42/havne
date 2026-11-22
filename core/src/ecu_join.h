// File/layer: core/src/ecu_join.h | internal, C++17, Qt-free, header-only
// Testing: UNIT: PluginWrapTest (test_signals); FUZZ: fuzz_plugin_wrap
//
// ECUs joined by their addresses (ADR-0064, ADR-0065 section 9): two ids
// at one rx and tx are one module, and an id another module holds takes
// "<id>_2". One join for a variant put into a plugin and for signal files
// merged into one set, so both name a module alike.

#ifndef ECUIO_ECU_JOIN_H
#define ECUIO_ECU_JOIN_H

#include "ecuio/signal_sets.h"
#include "ecuio/signals.h"
#include "json.h"
#include "signal_file_build.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace ecuio_ecus {

/* An ECU by its addresses: two ids at one rx and tx are one module */
struct address {
    uint32_t rx = 0;
    uint32_t tx = 0;

    bool operator==(const address& o) const { return rx == o.rx && tx == o.tx; }
};

struct ecu {
    std::string id;
    address addr;
};

inline std::vector<ecu> ecus_of(const ecuio_json::Value* list)
{
    std::vector<ecu> out;
    if (list != nullptr) {
        for (const ecuio_json::Value& e : list->items) {
            ecu x;
            x.id = e.find("id")->text;
            ecuio_sf::ecu_addresses(e, &x.addr.rx, &x.addr.tx);
            out.push_back(std::move(x));
        }
    }
    return out;
}

/* What a signal naming no ECU reads: the manifest's first, or with none
 * the first that the set a file extends asks */
inline address fallback_of(const std::vector<ecu>& ecus)
{
    if (!ecus.empty()) {
        return ecus.front().addr;
    }
    const ecuio_channel_decl_t& c = ecuio_obd2_signal_set()->channels[0];
    address a;
    a.rx = c.rx_id;
    a.tx = c.tx_id;
    return a;
}

/* The ECUs joined so far, in the order they came */
class joined
{
public:
    explicit joined(std::vector<ecu> have) : have_(std::move(have)) {}

    const ecu* at_address(const address& a) const
    {
        for (const ecu& e : have_) {
            if (e.addr == a) {
                return &e;
            }
        }
        return nullptr;
    }

    bool taken(const std::string& id) const
    {
        for (const ecu& e : have_) {
            if (e.id == id) {
                return true;
            }
        }
        return false;
    }

    /* "<id>_2", "_3" and on, the id cut so an ECU id's 31 bytes hold it */
    std::string free_id(const std::string& id) const
    {
        for (unsigned n = 2;; ++n) {
            const std::string tail = "_" + std::to_string(n);
            const std::string candidate = id.substr(0, 31 - tail.size()) + tail;
            if (!taken(candidate)) {
                return candidate;
            }
        }
    }

protected:
    std::vector<ecu> have_;
};

} // namespace ecuio_ecus

#endif /* ECUIO_ECU_JOIN_H */
