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

journal_found journal_find(const std::string& folder, uint32_t rx,
                           std::string* why)
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
    ecuio_json::Value root;
    ecuio_json::Error error;
    ecuio_json::Limits limits;
    limits.max_bytes = kJournalMax;
    limits.max_depth = 4;
    const ecuio_json::Value* state = nullptr;
    const ecuio_json::Value* format = nullptr;
    if (ecuio_json::parse(bytes.data(), bytes.size(), &root, &error, limits) &&
        root.type == ecuio_json::Type::object) {
        state = root.find("state");
        format = root.find("havne_journal");
    }
    const bool known = format != nullptr && state != nullptr &&
                       format->type == ecuio_json::Type::number &&
                       format->number == 1 &&
                       state->type == ecuio_json::Type::string;
    if (!known) {
        *why = "the flash journal " + name +
               " isn't one this Havne reads: the module may be mid-flash";
        return journal_found::failed;
    }
    if (state->text == "writing") {
        *why = "the flash journal " + name +
               " says a flash of this module didn't finish: the module may "
               "be mid-flash";
        return journal_found::unfinished;
    }
    return journal_found::none;
}

journal_writer::~journal_writer()
{
    close(false);
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
                          std::string* why)
{
    try {
        path_ = journal_path(folder, head.rx);
        head_ = std::move(head);
        if (!write(journal_mark{}, why)) {
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
        ecuio_files::remove_file(path_);
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
}

} // namespace ecuio_operation
