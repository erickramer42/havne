#include "operation_journal.h"

#include "folder_files.h"
#include "json.h"
#include "store_file.h"

#include <cstdio>
#include <cstring>
#include <utility>

namespace ecuio_operation {

namespace {

const char* state_word(journal_state s)
{
    switch (s) {
    case journal_state::writing:
        return "writing";
    case journal_state::done:
        return "done";
    default:
        return "ready";
    }
}

std::string text(const std::string& s)
{
    std::string out = "\"";
    for (const char c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (static_cast<unsigned char>(c) < 0x20) {
            char u[8];
            std::snprintf(u, sizeof u, "\\u%04X",
                          static_cast<unsigned>(static_cast<unsigned char>(c)));
            out += u;
        } else {
            out += c;
        }
    }
    return out + "\"";
}

std::string hex_of(uint64_t v, int digits)
{
    char text[24];
    std::snprintf(text, sizeof text, "%0*llX", digits,
                  static_cast<unsigned long long>(v));
    return text;
}

} // namespace

std::string journal_path(const std::string& folder, uint32_t rx)
{
    return folder + "/" + hex_of(rx, 8) + ".havnejournal";
}

std::string journal_text(const journal_head& h, const journal_mark& m)
{
    std::string t = "{\n  \"havne_journal\": 1,\n  \"state\": \"";
    t += state_word(m.state);
    t += "\",\n  \"operation\": " + text(h.operation);
    t += ",\n  \"plugin\": " + text(h.fingerprint);
    t += ",\n  \"rx\": \"" + hex_of(h.rx, 3) + "\"";
    t += ",\n  \"copy\": ";
    t += h.copy.empty() ? std::string("null") : text(h.copy);
    t += ",\n  \"utc\": " + std::to_string(h.utc);
    t += ",\n  \"erased\": " + std::to_string(m.erased);
    if (h.recoveries > 0) {
        t += ",\n  \"recoveries\": " + std::to_string(h.recoveries);
    }
    t += ",\n  \"images\": [";
    for (size_t i = 0; i < h.images.size(); ++i) {
        t += (i == 0 ? "" : ", ") + text(h.images[i]);
    }
    t += "],\n  \"sectors\": [";
    for (size_t i = 0; i < h.sectors.size(); ++i) {
        t += i == 0 ? "\n" : ",\n";
        t += "    {\"address\": \"" + hex_of(h.sectors[i].address, 8) +
             "\", \"bytes\": " + std::to_string(h.sectors[i].bytes) +
             ", \"written\": " + (i < m.written ? "true" : "false") + "}";
    }
    t += h.sectors.empty() ? "]\n}\n" : "\n  ]\n}\n";
    return t;
}

namespace {

using ecuio_json::Type;
using ecuio_json::Value;

bool hex_value(const Value* v, size_t most, uint64_t* out)
{
    if (v == nullptr || v->type != Type::string || v->text.empty() ||
        v->text.size() > most) {
        return false;
    }
    uint64_t n = 0;
    for (const char c : v->text) {
        const int d = c >= '0' && c <= '9'   ? c - '0'
                      : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                             : -1;
        if (d < 0) {
            return false;
        }
        n = (n << 4) | static_cast<uint64_t>(d);
    }
    *out = n;
    return true;
}

bool whole(const Value* v, uint64_t most, uint64_t* out)
{
    if (v == nullptr || v->type != Type::number || !(v->number >= 0) ||
        v->number > static_cast<double>(most) ||
        v->number != static_cast<double>(static_cast<uint64_t>(v->number))) {
        return false;
    }
    *out = static_cast<uint64_t>(v->number);
    return true;
}

/* written sectors come first: a mark counts them from the start */
bool sectors_of(const Value* list, journal_record* out)
{
    if (list == nullptr || list->type != Type::array ||
        list->items.size() > ECUIO_PLUGIN_MAX_SECTORS) {
        return false;
    }
    bool gap = false;
    for (const Value& v : list->items) {
        sector c;
        uint64_t bytes = 0;
        const Value* written =
            v.type == Type::object ? v.find("written") : nullptr;
        if (written == nullptr || written->type != Type::boolean ||
            !hex_value(v.find("address"), 8, &c.address) ||
            !whole(v.find("bytes"), UINT32_MAX, &bytes) || bytes == 0 ||
            (written->boolean && gap)) {
            return false;
        }
        c.bytes = bytes;
        gap = gap || !written->boolean;
        out->mark.written += written->boolean ? 1u : 0u;
        out->head.sectors.push_back(c);
    }
    return true;
}

bool images_of(const Value* list, journal_head* h)
{
    if (list == nullptr || list->type != Type::array ||
        list->items.size() > ECUIO_PLUGIN_MAX_STEPS) {
        return false;
    }
    for (const Value& i : list->items) {
        if (i.type != Type::string || i.text.size() != 64) {
            return false;
        }
        h->images.push_back(i.text);
    }
    return true;
}

bool head_of(const Value& root, uint32_t rx, journal_record* out)
{
    const Value* operation = root.find("operation");
    const Value* plugin = root.find("plugin");
    const Value* copy = root.find("copy");
    const Value* recoveries = root.find("recoveries");
    uint64_t at_rx = 0;
    uint64_t utc = 0;
    uint64_t n = 0;
    if (operation == nullptr || operation->type != Type::string ||
        plugin == nullptr || plugin->type != Type::string ||
        plugin->text.size() != 64 || !hex_value(root.find("rx"), 8, &at_rx) ||
        at_rx != rx || copy == nullptr ||
        (copy->type != Type::string && copy->type != Type::null) ||
        !whole(root.find("utc"), INT64_MAX, &utc) ||
        (recoveries != nullptr && !whole(recoveries, UINT32_MAX, &n))) {
        return false;
    }
    journal_head& h = out->head;
    h.operation = operation->text;
    h.fingerprint = plugin->text;
    h.rx = rx;
    h.copy = copy->type == Type::string ? copy->text : "";
    h.utc = static_cast<int64_t>(utc);
    h.recoveries = static_cast<uint32_t>(n);
    if (!images_of(root.find("images"), &h) ||
        !sectors_of(root.find("sectors"), out)) {
        return false;
    }
    const uint64_t all = h.sectors.size();
    uint64_t erased = all;
    const Value* e = root.find("erased");
    if (e != nullptr &&
        (!whole(e, all, &erased) || erased < out->mark.written)) {
        return false;
    }
    out->mark.erased = static_cast<uint32_t>(erased);
    return true;
}

journal_found journal_read(const std::string& folder, uint32_t rx,
                           journal_record* out, std::string* why)
{
    std::string bytes;
    std::string read_why;
    const ecuio_store_file::file_read r = ecuio_store_file::read_file(
        journal_path(folder, rx), kJournalMax, 0, &bytes, &read_why);
    if (r == ecuio_store_file::file_read::none) {
        return journal_found::none;
    }
    const std::string name = hex_of(rx, 8) + ".havnejournal";
    if (r == ecuio_store_file::file_read::failed) {
        *why = "the flash journal " + name + " " + read_why;
        return journal_found::failed;
    }
    const std::string unread =
        "the flash journal " + name +
        " isn't one this Havne reads: the module may be mid-flash";
    Value root;
    ecuio_json::Error error;
    ecuio_json::Limits limits;
    limits.max_bytes = kJournalMax;
    limits.max_depth = 4;
    const Value* state = nullptr;
    const Value* format = nullptr;
    if (ecuio_json::parse(bytes.data(), bytes.size(), &root, &error, limits) &&
        root.type == Type::object) {
        state = root.find("state");
        format = root.find("havne_journal");
    }
    const bool known = format != nullptr && state != nullptr &&
                       format->type == Type::number && format->number == 1 &&
                       state->type == Type::string;
    if (!known) {
        *why = unread;
        return journal_found::failed;
    }
    if (state->text == "writing") {
        if (out != nullptr && !head_of(root, rx, out)) {
            *out = journal_record{};
            *why = unread;
            return journal_found::failed;
        }
        if (out != nullptr) {
            out->mark.state = journal_state::writing;
        }
        *why = "the flash journal " + name +
               " says a flash of this module didn't finish: the module may "
               "be mid-flash";
        return journal_found::unfinished;
    }
    if (state->text != "ready" && state->text != "done") {
        *why = unread;
        return journal_found::failed;
    }
    return journal_found::none;
}

} // namespace

journal_found journal_find(const std::string& folder, uint32_t rx,
                           std::string* why)
{
    return journal_read(folder, rx, nullptr, why);
}

journal_found journal_load(const std::string& folder, uint32_t rx,
                           journal_record* out, std::string* why)
{
    *out = journal_record{};
    return journal_read(folder, rx, out, why);
}

journal_writer::~journal_writer()
{
    try {
        close(false);
        // NOLINTNEXTLINE(bugprone-empty-catch): the join fails only on a bad thread
    } catch (const std::exception&) {
    }
}

bool journal_writer::write(const journal_mark& m, std::string* why)
{
    bool renamed = false;
    const int fail = journal_test_fail_at.exchange(0);
    try {
        return ecuio_store_file::replace_whole(path_, journal_text(head_, m),
                                               fail, nullptr, &renamed, why);
    } catch (const std::bad_alloc&) {
        *why = "out of memory";
        return false;
    }
}

bool journal_writer::open(const std::string& folder, journal_head head,
                          std::string* why, const journal_mark& first)
{
    try {
        path_ = journal_path(folder, head.rx);
        head_ = std::move(head);
        if (!write(first, why)) {
            *why = "the flash journal can't be written: " + *why;
            return false;
        }
        posted_.store(0);
        flushed_.store(0);
        failed_.store(false);
        stop_ = false;
        thread_ = std::thread(&journal_writer::loop, this);
        return true;
    } catch (const std::exception&) {
        if (first.state == journal_state::ready) {
            ecuio_files::remove_file(path_);
        }
        *why = "the flash journal's writer can't start";
        return false;
    }
}

void journal_writer::post(const journal_mark& m)
{
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        pending_ = m;
        posted_.store(posted_.load(std::memory_order_relaxed) + 1,
                      std::memory_order_release);
    }
    wake_.notify_one();
}

void journal_writer::loop()
{
    for (;;) {
        journal_mark m;
        uint32_t target = 0;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [&] {
                return stop_ || posted_.load(std::memory_order_relaxed) !=
                                    flushed_.load(std::memory_order_relaxed);
            });
            if (posted_.load(std::memory_order_relaxed) ==
                flushed_.load(std::memory_order_relaxed)) {
                return;
            }
            m = pending_;
            target = posted_.load(std::memory_order_relaxed);
        }
        std::string why;
        if (!write(m, &why)) {
            std::snprintf(words_, sizeof words_,
                          "the flash journal can't be written: %s",
                          why.c_str());
            failed_.store(true, std::memory_order_release);
            return;
        }
        flushed_.store(target, std::memory_order_release);
    }
}

void journal_writer::close(bool remove)
{
    if (thread_.joinable()) {
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        wake_.notify_one();
        thread_.join();
    }
    if (remove && !path_.empty()) {
        ecuio_files::remove_file(path_);
    }
    path_.clear();
}

} // namespace ecuio_operation
