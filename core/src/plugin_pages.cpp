// File/layer: core/src/plugin_pages.cpp | C++17, Qt-free, every platform
// Testing: UNIT: PluginPagesTest, PluginWrapTest (test_signals); FUZZ:
//          fuzz_plugin_manifest
//
// The manifest's "pages" (ADR-0052's appendix on pages): the Dashboard
// pages a plugin carries, each an ADR-0057 layout file of one page for one
// form. Core checks the envelope (an object, "format" 2, a form, one page,
// no "files") and writes the file out for the host; the page's cells are
// the dashboard's rules, which the app and havne-cli's layout check judge.

#include "plugin_loader.h"

#include <cstring>
#include <string>
#include <utility>

namespace ecuio_plugin_loader {

namespace {

/* The bytes ecuio_json::write gives for v at depth, but a number's
 * digits past one and a string's escapes, counted only until they pass
 * limit: a large page is refused before it is written */
size_t written_at_least(const Value& v, size_t depth, size_t limit)
{
    switch (v.type) {
    case Type::string:
        return v.text.size() + 2;
    case Type::null:
        return 4;
    case Type::boolean:
        return v.boolean ? 4 : 5;
    case Type::number:
        return 1;
    default:
        break;
    }
    const size_t count = v.items.size() + v.members.size();
    if (count == 0) {
        return 2;
    }
    /* Each item's indented line and its end, the commas, "[\n" and the
     * close's own indented line */
    const size_t line = 2 * (depth + 1) + 1;
    size_t n = 2 + (count - 1) + 2 * depth + 1;
    for (const Value& item : v.items) {
        n += line + written_at_least(item, depth + 1, limit);
        if (n > limit) {
            return n;
        }
    }
    for (const Member& m : v.members) {
        n += line + m.key.size() + 4 +
             written_at_least(m.value, depth + 1, limit);
        if (n > limit) {
            return n;
        }
    }
    return n;
}

} // namespace

/* "pages": each path once, each file's envelope, in the manifest's order.
 * A packed page leaves files_ here, before the variants read it */
void loader::read_pages(const Value& root, ecuio_plugin& p)
{
    if (const Value* list = root.find("pages")) {
        expect(*list, Type::array, "\"pages\"");
        if (list->items.empty()) {
            refuse(*list, "\"pages\" lists none");
        }
        if (list->items.size() > ECUIO_PLUGIN_MAX_PAGES) {
            refuse(*list, "\"pages\" lists more than 16");
        }
        for (const Value& item : list->items) {
            const std::string& path = path_of(item, "a page");
            if (is_page(path)) {
                refuse(item, "page " + q(path) + " is listed twice");
            }
            page_paths_.push_back(path);
        }
        for (size_t i = 0; i < page_paths_.size(); ++i) {
            page out;
            out.path = page_paths_[i];
            check_page(page_root(list->items[i], out.path), out);
            p.pages.push_back(std::move(out));
        }
        ecuio_least::raise(p.least, ecuio_least::kPages);
    }
    if (packed() && files_.size() + binaries_.size() > ECUIO_PLUGIN_MAX_FILES) {
        refuse(*root.find("files"), "\"files\" holds more than 64");
    }
}

/* The page at path: a packed one taken out of files_, a folder's read
 * through the host's callback and parsed */
const Value& loader::page_root(const Value& at, const std::string& path)
{
    if (packed()) {
        for (auto it = files_.begin(); it != files_.end(); ++it) {
            if (it->path == path) {
                const Value* root = it->root;
                files_.erase(it);
                return *root;
            }
        }
        for (const binary_file& b : binaries_) {
            if (b.path == path) {
                refuse(at, "page " + q(path) +
                               " is a binary file: a page is a layout file's "
                               "JSON");
            }
        }
        refuse(at, "page " + q(path) + " isn't in \"files\"");
    }
    const char* bytes = nullptr;
    size_t length = 0;
    if (read_(user_, path.c_str(), &bytes, &length) != 0 ||
        (bytes == nullptr && length > 0)) {
        refuse(at, "page " + q(path) + " couldn't be read");
    }
    if (length > ECUIO_PLUGIN_PAGE_MAX_BYTES) {
        refuse(at, "page " + q(path) + " is over 82,400 bytes");
    }
    total_ += length;
    if (total_ > ECUIO_PLUGIN_PACKAGE_MAX) {
        refuse(at, "the package is over 16 MiB");
    }
    page_values_.emplace_back();
    ecuio_json::Error error;
    ecuio_json::Limits limits;
    limits.max_bytes = ECUIO_PLUGIN_PAGE_MAX_BYTES;
    if (!ecuio_json::parse(bytes != nullptr ? bytes : "", length,
                           &page_values_.back(), &error, limits)) {
        throw placed_json(path + ", ", error);
    }
    return page_values_.back();
}

/* A layout file of one page for one form, and its text as written */
void loader::check_page(const Value& root, page& out)
{
    expect(root, Type::object, "a page file");
    const Value* format = root.find("format");
    if (format == nullptr) {
        refuse(root, "a page file needs \"format\": 2");
    }
    if (format->type != Type::number || !format->integer ||
        format->whole != 2) {
        refuse(*format, "a page file is a layout file of \"format\" 2");
    }
    const Value* form = root.find("form");
    if (form == nullptr) {
        refuse(root, "a page file needs \"form\": \"phone\" or \"desktop\"");
    }
    expect(*form, Type::string, "\"form\"");
    if (form->text == "phone") {
        out.form = ECUIO_PLUGIN_PAGE_PHONE;
    } else if (form->text == "desktop") {
        out.form = ECUIO_PLUGIN_PAGE_DESKTOP;
    } else {
        refuse(*form,
               "\"form\" is \"phone\" or \"desktop\", not " + q(form->text));
    }
    if (const Value* files = root.find("files")) {
        expect(*files, Type::array, "\"files\"");
        if (!files->items.empty()) {
            refuse(*files, "a page file lists no \"files\": the plugin is the "
                           "file its signals need");
        }
    }
    const Value* pages = root.find("pages");
    if (pages == nullptr) {
        refuse(root, "a page file needs \"pages\": one page");
    }
    expect(*pages, Type::array, "\"pages\"");
    if (pages->items.size() != 1) {
        refuse(*pages, "a page file holds one page, not " +
                           std::to_string(pages->items.size()));
    }
    expect(pages->items.front(), Type::object, "a page");
    const char* const over = "a page file is over 82,400 bytes as written";
    /* The writing ends in a newline */
    if (written_at_least(root, 0, ECUIO_PLUGIN_PAGE_MAX_BYTES) + 1 >
        ECUIO_PLUGIN_PAGE_MAX_BYTES) {
        refuse(root, over);
    }
    out.text = ecuio_json::write(root);
    if (out.text.size() > ECUIO_PLUGIN_PAGE_MAX_BYTES) {
        refuse(root, over);
    }
}

bool loader::is_page(const std::string& path) const
{
    for (const std::string& known : page_paths_) {
        if (known == path) {
            return true;
        }
    }
    return false;
}

} // namespace ecuio_plugin_loader

ecuio_status_t ecuio_plugin_page(const ecuio_plugin_t* plugin, uint32_t index,
                                 ecuio_plugin_page_t* out)
{
    if (plugin == nullptr || out == nullptr) {
        return ECUIO_ERR_NULL_ARGUMENT;
    }
    if (out->struct_size < sizeof(ecuio_plugin_page_t) ||
        index >= plugin->pages.size()) {
        return ECUIO_ERR_INVALID_ARGUMENT;
    }
    const ecuio_plugin_loader::page& from = plugin->pages[index];
    ecuio_plugin_page_t page{};
    page.struct_size = sizeof(ecuio_plugin_page_t);
    page.form = from.form;
    page.text_bytes = static_cast<uint32_t>(from.text.size());
    std::memcpy(page.path, from.path.c_str(), from.path.size() + 1);
    *out = page;
    return ECUIO_OK;
}

const char* ecuio_plugin_page_text(const ecuio_plugin_t* plugin, uint32_t index)
{
    if (plugin == nullptr || index >= plugin->pages.size()) {
        return nullptr;
    }
    return plugin->pages[index].text.c_str();
}
