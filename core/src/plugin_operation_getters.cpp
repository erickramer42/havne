#include "operation_plan.h"
#include "plugin_loader.h"

namespace {

using ecuio_operation::plan;

/* The plan a getter names, or NULL: none without the tier */
const plan* plan_at(const ecuio_plugin_t* plugin, uint32_t operation)
{
    return operation < plugin->operations.size()
               ? &plugin->operations[operation]
               : nullptr;
}

} // namespace

extern "C" {

ecuio_status_t ecuio_plugin_operation_count(const ecuio_plugin_t* plugin,
                                            uint32_t* count)
{
    if (plugin == nullptr || count == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    *count = static_cast<uint32_t>(plugin->operations.size());
    return ECUIO_OK;
}

ecuio_status_t ecuio_plugin_operation(const ecuio_plugin_t* plugin,
                                      uint32_t index,
                                      ecuio_plugin_operation_t* out)
{
    if (plugin == nullptr || out == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    const plan* p = plan_at(plugin, index);
    if (p == nullptr || out->struct_size < sizeof(ecuio_plugin_operation_t)) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    ecuio_plugin_operation_t o{};
    o.struct_size = sizeof o;
    o.kind = p->kind;
    o.op_class = ecuio_operation::class_of(*p);
    o.rx_id = p->rx_id;
    o.tx_id = p->tx_id;
    o.level = p->level;
    o.step_count =
        static_cast<uint32_t>(p->steps.size()) + (p->has_kernel ? 2u : 0u);
    o.ident_count = static_cast<uint32_t>(p->idents.size());
    o.unlock_sessions = ecuio_operation::unlock_sessions(*p);
    o.image_bytes = ecuio_operation::image_bytes(*p);
    o.leave = p->leave;
    ecuio_sf::copy_text(o.id, sizeof o.id, p->id);
    ecuio_sf::copy_text(o.title, sizeof o.title, p->title);
    *out = o;
    return ECUIO_OK;
}

ecuio_status_t ecuio_plugin_operation_step(const ecuio_plugin_t* plugin,
                                           uint32_t operation, uint32_t index,
                                           ecuio_plugin_operation_step_t* out)
{
    if (plugin == nullptr || out == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    const plan* p = plan_at(plugin, operation);
    const uint32_t lead = p != nullptr && p->has_kernel ? 2u : 0u;
    if (p == nullptr || index >= p->steps.size() + lead ||
        out->struct_size < sizeof(ecuio_plugin_operation_step_t)) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    ecuio_plugin_operation_step_t s{};
    s.struct_size = sizeof s;
    if (index < lead) {
        const ecuio_operation::kernel& k = p->helper;
        s.session = ECUIO_STEP_SESSION_PROGRAMMING;
        if (index == 0) {
            s.kind = ECUIO_STEP_DOWNLOAD;
            s.address = k.address;
            s.bytes = k.blob.size();
            s.address_bytes = k.address_bytes;
            s.size_bytes = k.size_bytes;
            ecuio_sf::copy_text(s.name, sizeof s.name, k.download_name);
        } else {
            s.kind = ECUIO_STEP_ROUTINE;
            s.routine_id = k.routine;
            ecuio_sf::copy_text(s.name, sizeof s.name, k.routine_name);
        }
        *out = s;
        return ECUIO_OK;
    }
    const ecuio_operation::step& from = p->steps[index - lead];
    s.kind = from.kind;
    s.session = from.session;
    s.address = from.address;
    s.bytes = from.bytes;
    s.chunk = from.chunk;
    s.address_bytes = from.address_bytes;
    s.size_bytes = from.size_bytes;
    s.dfi = from.dfi;
    ecuio_sf::copy_text(s.name, sizeof s.name, from.name);
    *out = s;
    return ECUIO_OK;
}

ecuio_status_t ecuio_plugin_operation_ident(const ecuio_plugin_t* plugin,
                                            uint32_t operation, uint32_t index,
                                            ecuio_plugin_operation_ident_t* out)
{
    if (plugin == nullptr || out == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    const plan* p = plan_at(plugin, operation);
    if (p == nullptr || index >= p->idents.size() ||
        out->struct_size < sizeof(ecuio_plugin_operation_ident_t)) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    ecuio_plugin_operation_ident_t d{};
    d.struct_size = sizeof d;
    d.did = p->idents[index].did;
    d.role = p->idents[index].role;
    *out = d;
    return ECUIO_OK;
}

ecuio_status_t
ecuio_plugin_operation_kernel(const ecuio_plugin_t* plugin, uint32_t operation,
                              ecuio_plugin_operation_kernel_t* out)
{
    if (plugin == nullptr || out == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    const plan* p = plan_at(plugin, operation);
    if (p == nullptr || !p->has_kernel ||
        out->struct_size < sizeof(ecuio_plugin_operation_kernel_t)) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    const ecuio_operation::kernel& from = p->helper;
    ecuio_plugin_operation_kernel_t k{};
    k.struct_size = sizeof k;
    k.ram_address = from.ram_address;
    k.ram_bytes = from.ram_bytes;
    k.start_count = static_cast<uint32_t>(from.starts.size());
    for (size_t i = 0; i < from.starts.size(); ++i) {
        k.start[i] = from.starts[i];
    }
    k.quiet_session = from.quiet_session;
    k.quiet_keepalive = from.quiet_keepalive;
    k.keepalive_ms = from.keepalive_ms;
    k.stays = from.stays ? 1u : 0u;
    *out = k;
    return ECUIO_OK;
}

ecuio_status_t ecuio_plugin_operation_blob(const ecuio_plugin_t* plugin,
                                           uint32_t operation,
                                           const uint8_t** bytes,
                                           uint64_t* length)
{
    if (plugin == nullptr || bytes == nullptr || length == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    const plan* p = plan_at(plugin, operation);
    if (p == nullptr || !p->has_kernel) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    *bytes = p->helper.blob.data();
    *length = p->helper.blob.size();
    return ECUIO_OK;
}

} // extern "C"
