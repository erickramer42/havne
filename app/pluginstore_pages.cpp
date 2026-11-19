// File/layer: app/pluginstore_pages.cpp | Qt6 Core, app layer, C++17
// Testing: HARNESS: BridgeTest.PluginPages* (test_bridge)
//
// The Dashboard pages a plugin carries (ADR-0052's appendix on pages): the
// store's entries list them and hand out their text, which core holds for
// as long as the plugin is loaded. The store judges nothing of a page: the
// Dashboard reads it as it reads any layout file.

#include "pluginstore.h"

#include "ecuio/plugin.h"

QVariantList PluginStore::pagesOf(const ecuio_plugin_t* plugin)
{
    QVariantList out;
    ecuio_plugin_info_t info{};
    info.struct_size = sizeof info;
    if (ecuio_plugin_info(plugin, &info) != ECUIO_OK) {
        return out;
    }
    for (uint32_t i = 0; i < info.page_count; ++i) {
        ecuio_plugin_page_t page{};
        page.struct_size = sizeof page;
        if (ecuio_plugin_page(plugin, i, &page) != ECUIO_OK) {
            continue;
        }
        const bool phone = page.form == ECUIO_PLUGIN_PAGE_PHONE;
        out << QVariantMap{
            {QStringLiteral("index"), static_cast<int>(i)},
            {QStringLiteral("form"),
             phone ? QStringLiteral("phone") : QStringLiteral("desktop")},
            {QStringLiteral("path"), QString::fromUtf8(page.path)}};
    }
    return out;
}

QString PluginStore::pageText(const QString& id, int index)
{
    (void)all(); // the kept copies loaded first
    const auto held = heldWithId(id);
    if (held == m_held.end()) {
        return {};
    }
    /* a negative index is past any count */
    const char* text = ecuio_plugin_page_text(held->second.plugin,
                                              static_cast<uint32_t>(index));
    return text == nullptr ? QString() : QString::fromUtf8(text);
}
