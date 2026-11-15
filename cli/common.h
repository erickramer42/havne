// File/layer: cli/common.h | havne-cli, what every command shares: the exit codes,
//             the interrupt, standard output, the file calls
// Testing: UNIT: CliGoldenTest, CliUsageTest, CliExitTest, CliPathTest,
//          CliTransferRunTest (ctest); CliPartialTest (test_cli)
//
// Public headers only: what any program linking core can do.

#ifndef HAVNE_CLI_COMMON_H
#define HAVNE_CLI_COMMON_H

#include "jsonl.h"

#include "ecuio/core.h"

#include <atomic>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <sys/stat.h> /* stat: a plugin's folder */
#include <unistd.h>   /* read: the marks typed into stream --record */
#endif

// play writes to a SocketCAN interface: Linux only, as socketcan:// is
#if defined(__linux__) && !defined(__ANDROID__)
#define HAVNE_CLI_PLAY 1
#else
#define HAVNE_CLI_PLAY 0
#endif

namespace cmd {

constexpr int kExitOk = 0;
constexpr int kExitFailed = 1;
constexpr int kExitUsage = 2;
constexpr int kExitConnect = 3;

constexpr int kSchema = 1;

// A frame line at ECUIO_FRAME_DATA_MAX bytes takes about 12.4 KB; a
// values line with text values can take more, and is cut to fit
constexpr size_t kLineBytes = 32 * 1024;
constexpr auto kPoll = std::chrono::milliseconds(100);

unsigned long env_ms(const char* name, unsigned long fallback);

extern std::atomic<bool> g_interrupted;

#ifdef _WIN32
extern std::atomic<bool> g_done;
#endif

void second_interrupt_over(const char* where = nullptr);
void install_handlers(bool secondEnds = false);

// Standard output, JSON lines only. A failure is remembered: a closed
// pipe (EPIPE) ends the stream quietly, anything else is an error.
struct Output {
    bool failed = false;
    bool closed = false;
    int error = 0;

    // False after a failure that wasn't a closed pipe, which it reports
    bool ok() const
    {
        if (failed && !closed) {
            std::fprintf(stderr, "havne-cli: writing the output failed: %s\n",
                         std::generic_category().message(error).c_str());
            return false;
        }
        return true;
    }

    void write(const char* p, size_t n)
    {
        if (failed || n == 0) {
            return;
        }
        if (std::fwrite(p, 1, n, stdout) != n) {
            fail();
        }
    }
    void flush()
    {
        if (!failed && std::fflush(stdout) != 0) {
            fail();
        }
    }

private:
    void fail()
    {
        failed = true;
        error = errno;
        closed = errno == EPIPE;
#ifdef _WIN32
        // A pipe whose reader has gone fails WriteFile with ERROR_NO_DATA
        // (Git Bash's head, PowerShell's Select-Object -First), which the
        // CRT reports as EINVAL; ERROR_BROKEN_PIPE is EPIPE already
        const DWORD last = GetLastError();
        closed = closed || last == ERROR_NO_DATA || last == ERROR_BROKEN_PIPE;
#endif
    }
};

// A line built on the main thread, which may be long (a set's defs)
struct BigLine {
    explicit BigLine(size_t cap) : buf(cap), line(buf.data(), buf.size()) {}
    std::vector<char> buf;
    cli::Line line;
};

void write_line(Output& out, cli::Line& l);
int ended(Output& out, int code);
int export_interrupted(void* user);
int import_interrupted(void* user);
int health_interrupted(void* user);
int interval_interrupted(void* user);
int open_interrupted(uint64_t, uint64_t, void* user);
int view_interrupted(void* user);
int view_second_interrupted(void* user);
int view_open_interrupted(uint64_t, uint64_t, void*);
int ended_interrupted(Output& out, const char* where = nullptr);
void write_start(Output& out);

// An error line, with core's detail for the last connect unless given
// one of the CLI's own
void write_error(Output& out, ecuio_status_t status,
                 const char* detail = nullptr);

#ifdef _WIN32
std::wstring wide(const char* utf8);
#endif

bool read_whole(const char* path, size_t max, const char* what,
                std::string* bytes, std::string* why);

bool is_folder(const char* path);

bool write_whole(const char* path, const std::string& bytes, std::string* why,
                 const char* what = "the converted file");
bool is_file(const char* path);

} // namespace cmd

#endif // HAVNE_CLI_COMMON_H
