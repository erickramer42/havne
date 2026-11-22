// File/layer: core/src/signal_file_readers.cpp | C++17, Qt-free, every platform
// Testing: UNIT: SignalFileFormat3Test (test_signals), c_abi_merge
//
// What a loaded signal file says of how its set is made up (signals.h,
// ADR-0065 section 9): its modules as its "ecus" entries name them. Each
// call only reads, so any thread may make it while the file lives.

#include "ecuio/signals.h"
#include "error_detail.h"
#include "signal_file_build.h"

#include <cstring>
#include <string>

namespace {

/* Whether a struct_size holds a struct of size bytes; the detail names
 * the struct as what when it doesn't */
bool sized(uint32_t struct_size, size_t size, const char* what)
{
    if (struct_size >= size) {
        return true;
    }
    ecuio_detail_set("the %s's struct_size is %u, under its %u bytes", what,
                     static_cast<unsigned>(struct_size),
                     static_cast<unsigned>(size));
    return false;
}

} // namespace

extern "C" {

ecuio_status_t ecuio_signal_file_module(const ecuio_signal_file_t* file,
                                        uint32_t index,
                                        ecuio_signal_module_t* out)
{
    ecuio_detail_clear();
    if (file == nullptr || out == nullptr) {
        ecuio_detail_set("the module needs a file and a struct to fill");
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    if (!sized(out->struct_size, sizeof(ecuio_signal_module_t), "module")) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    if (index >= file->channels.size() || index >= file->modules.size()) {
        ecuio_detail_set("the file's set has %u modules, and index %u is none",
                         static_cast<unsigned>(file->channels.size()),
                         static_cast<unsigned>(index));
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    const ecuio_signal_file::module& m = file->modules[index];
    ecuio_signal_module_t filled{};
    filled.struct_size = sizeof(ecuio_signal_module_t);
    filled.flags = m.id.empty() ? ECUIO_MODULE_BASE : 0u;
    filled.rx_id = file->channels[index].rx_id;
    filled.tx_id = file->channels[index].tx_id;
    filled.part = m.part;
    ecuio_sf::copy_text(filled.id, sizeof filled.id, m.id);
    ecuio_sf::copy_text(filled.name, sizeof filled.name, m.name);
    std::memcpy(out, &filled, sizeof filled);
    return ECUIO_OK;
}

} // extern "C"
