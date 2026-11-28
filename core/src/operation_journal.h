#ifndef ECUIO_OPERATION_JOURNAL_H
#define ECUIO_OPERATION_JOURNAL_H

#include "operation_plan.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ecuio_files {
class file_lock;
}

namespace ecuio_operation {

constexpr size_t kJournalMax = 64 * 1024;

struct journal_head {
    std::string operation;
    std::string fingerprint; /* the plugin's, 64 hex */
    uint32_t rx = 0;
    std::string copy; /* the copy the sectors were judged against; "" none */
    std::vector<std::string> images; /* each step's SHA-256, hex */
    std::vector<sector> sectors;     /* the ones the run writes */
    int64_t utc = 0;
    uint32_t recoveries = 0;
};

enum class journal_state : uint8_t { ready, writing, done };

struct journal_mark {
    journal_state state = journal_state::ready;
    uint32_t written = 0; /* of head.sectors, written whole */
    uint32_t erased = 0;  /* of head.sectors, their erase marked sent */
};

enum class journal_found : uint8_t { none, unfinished, failed };

struct journal_record {
    journal_head head;
    journal_mark mark;
};

std::string journal_path(const std::string& folder, uint32_t rx);
std::string journal_text(const journal_head& h, const journal_mark& m);

/* A journal a host kill left "writing" is unfinished; "ready" is no
 * write begun */
journal_found journal_find(const std::string& folder, uint32_t rx,
                           std::string* why);

/* An unfinished journal read whole; one whose fields don't read is failed */
journal_found journal_load(const std::string& folder, uint32_t rx,
                           journal_record* out, std::string* why);

/* TEST-ONLY: the step of the next write that fails, once (store_file's) */
inline std::atomic<int> journal_test_fail_at{0};

/* The journal's file, written by a thread of its own: the worker posts a
 * mark and waits for flushed() to reach posted() before its next request */
class journal_writer
{
public:
    journal_writer();
    journal_writer(const journal_writer&) = delete;
    journal_writer& operator=(const journal_writer&) = delete;
    ~journal_writer();

    bool open(const std::string& folder, journal_head head, std::string* why,
              const journal_mark& first = journal_mark{});
    void post(const journal_mark& m);
    uint32_t posted() const { return posted_.load(std::memory_order_acquire); }
    uint32_t flushed() const
    {
        return flushed_.load(std::memory_order_acquire);
    }
    bool failed() const { return failed_.load(std::memory_order_acquire); }
    /* open refused: another writer holds the journal's claim */
    bool busy() const { return busy_; }
    const char* words() const { return words_; }
    void close(bool remove);
    bool is_open() const { return thread_.joinable(); }

private:
    void loop();
    bool write(const journal_mark& m, std::string* why);

    std::string path_;
    journal_head head_;
    std::mutex mutex_;
    std::condition_variable wake_;
    journal_mark pending_;
    bool stop_ = false;
    std::atomic<uint32_t> posted_{0};
    std::atomic<uint32_t> flushed_{0};
    std::atomic<bool> failed_{false};
    bool busy_ = false;
    std::unique_ptr<ecuio_files::file_lock> claim_;
    char words_[160] = {};
    std::thread thread_;
};

} // namespace ecuio_operation

#endif /* ECUIO_OPERATION_JOURNAL_H */
