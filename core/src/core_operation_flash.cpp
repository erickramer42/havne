#include "operation_flash_start.h"

#include "ecuio/dump.h"
#include "conversation_registry.h"
#include "error_detail.h"
#include "operation_session.h"
#include "session.h"
#include "sha256.h"

#include <chrono>
#include <cstring>
#include <memory>

namespace ecuio_operation {

namespace {

ecuio_status_t refuse(ecuio_status_t status, const std::string& words)
{
    ecuio_detail_set("%s", words.c_str());
    return status;
}

struct dump_closer {
    void operator()(ecuio_dump_t* d) const { ecuio_dump_close(d); }
};

bool ranges_are_images(const ecuio_dump_t* d, const plan& p)
{
    ecuio_dump_info_t info{};
    info.struct_size = sizeof info;
    if (ecuio_dump_get_info(d, &info) != ECUIO_OK ||
        info.range_count != p.steps.size() ||
        info.image_bytes != image_bytes(p) || info.rx_id != p.rx_id) {
        return false;
    }
    for (uint32_t i = 0; i < info.range_count; ++i) {
        ecuio_dump_range_t r{};
        r.struct_size = sizeof r;
        if (ecuio_dump_get_range(d, i, &r) != ECUIO_OK ||
            r.address != p.steps[i].address || r.bytes != p.steps[i].bytes) {
            return false;
        }
    }
    return true;
}

} // namespace

ecuio_status_t flash_fits(const plan& p, uint32_t write_bound)
{
    uint32_t need = 3;
    for (const step& s : p.steps) {
        const uint32_t erase = 5u + s.address_bytes + s.size_bytes;
        need = erase > need ? erase : need;
    }
    if (write_bound < need) {
        ecuio_detail_set("the flash's erase is asked in %u bytes: this "
                         "adapter carries no write that long",
                         need);
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    return ECUIO_OK;
}

ecuio_status_t flash_current(const std::string& folder, const char* name,
                             const plan& p, std::vector<uint8_t>* out)
{
    const std::string n = name;
    if (n.empty() || n.find_first_of("/\\:") != std::string::npos) {
        return refuse(ECUIO_ERR_INVALID_ARGUMENT,
                      "the copy is named as the folder lists it, not by a "
                      "path");
    }
    ecuio_dump_t* opened = nullptr;
    if (ecuio_dump_open((folder + "/" + n).c_str(), &opened) != ECUIO_OK) {
        return refuse(ECUIO_ERR_INVALID_ARGUMENT,
                      "the copy " + n +
                          " can't be read: " + ecuio_last_error_detail());
    }
    const std::unique_ptr<ecuio_dump_t, dump_closer> d(opened);
    if (!ranges_are_images(d.get(), p)) {
        return refuse(ECUIO_ERR_INVALID_ARGUMENT,
                      "the copy " + n +
                          "'s ranges aren't this flash's images: its "
                          "sectors can't be judged by it");
    }
    if (ecuio_dump_verify(d.get(), nullptr, nullptr, nullptr, 0) != ECUIO_OK) {
        return refuse(ECUIO_ERR_INVALID_ARGUMENT,
                      "the copy " + n +
                          " doesn't verify: " + ecuio_last_error_detail());
    }
    out->resize(static_cast<size_t>(image_bytes(p)));
    size_t got = 0;
    if (ecuio_dump_read(d.get(), 0, out->data(), out->size(), &got) !=
            ECUIO_OK ||
        got != out->size()) {
        out->clear();
        return refuse(ECUIO_ERR_INVALID_ARGUMENT,
                      "the copy " + n + " can't be read whole");
    }
    return ECUIO_OK;
}

ecuio_status_t flash_journal(const ecuio_session* s, session_run& o,
                             const plan& p, const std::string& folder,
                             const char* copy, uint32_t* sectors,
                             uint64_t* bytes)
{
    std::string why;
    const journal_found found = journal_find(folder, p.rx_id, &why);
    if (found != journal_found::none) {
        return refuse(ECUIO_ERR_IN_USE, why);
    }
    flash_sector chosen[ECUIO_PLUGIN_MAX_SECTORS];
    const uint32_t n =
        flash_select(p, o.current.empty() ? nullptr : o.current.data(), chosen);
    journal_head h;
    h.operation = p.id;
    h.fingerprint = ecuio_sha256::hex(s->conversation->fingerprint);
    h.rx = p.rx_id;
    h.copy = copy != nullptr ? copy : "";
    for (const step& st : p.steps) {
        h.images.push_back(ecuio_sha256::hex(st.sha256));
    }
    *bytes = 0;
    for (uint32_t i = 0; i < n; ++i) {
        h.sectors.push_back({chosen[i].address, chosen[i].bytes});
        *bytes += chosen[i].bytes;
    }
    h.utc = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count();
    *sectors = n;
    o.journal.close(false);
    if (!o.journal.open(folder, std::move(h), &why)) {
        return refuse(ECUIO_ERR_LOAD_FAILED, why);
    }
    return ECUIO_OK;
}

} // namespace ecuio_operation
