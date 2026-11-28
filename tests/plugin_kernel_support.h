#ifndef ECUIO_TESTS_PLUGIN_KERNEL_SUPPORT_H
#define ECUIO_TESTS_PLUGIN_KERNEL_SUPPORT_H

#include "plugin_conversation_support.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace kernel_test {

using conversation_test::base64;
using conversation_test::bytes;
using conversation_test::Made;
using conversation_test::sha_of;

inline const char kPublisher[] =
    R"({"name": "Made-up Tools", "contact": "c", "license": "MIT", )"
    R"("terms": "t"})";

inline bytes helper(size_t n = 300)
{
    bytes b(n);
    for (size_t i = 0; i < n; ++i) {
        b[i] = static_cast<uint8_t>(i * 7 + 3);
    }
    return b;
}

/* A kernel read's parts as text, each a case's to change */
struct Kernel {
    bytes blob = helper();
    std::string ram = R"({"address": "00F00000", "bytes": 16384})";
    std::string start = R"(["0201"])";
    std::string stays = "true";
    std::string functional =
        R"([{"request": "10 82"}, {"request": "3E 80", "every_ms": 2000}])";
    std::string download_address = "\"00F00000\"";
    std::string sha; /* the download's sha256, when not the blob's own */
    std::string routine = "\"0201\"";
    std::string download_extra; /* keys after the download's own */
    std::string routine_extra;  /* and after the routine's */
    std::string kind = "\"kernel_read\"";
    std::string kernel_extra;
    std::string ranges =
        R"({"step": "upload", "name": "code", "session": "programming", )"
        R"("address": "00080000", "bytes": 4096, "address_bytes": 4, )"
        R"("size_bytes": 4, "dfi": "00"})";
    std::string lead; /* the steps before the ranges, when not the two */

    std::string kernel() const
    {
        std::string k = "{\"ram\": " + ram + ", \"start\": " + start +
                        ", \"stays\": " + stays;
        if (!functional.empty()) {
            k += ", \"functional\": " + functional;
        }
        return k + kernel_extra + "}";
    }

    std::string steps() const
    {
        if (!lead.empty()) {
            return "[" + lead + ranges + "]";
        }
        return "[" + download() + ", " + routine_step() + ", " + ranges + "]";
    }

    std::string download() const
    {
        return R"({"step": "download", "name": "helper", "session": )"
               R"("programming", "address": )" +
               download_address +
               R"(, "address_bytes": 4, "size_bytes": 4, "file": )"
               R"("kernel.bin", "sha256": ")" +
               (sha.empty() ? sha_of(blob) : sha) + "\"" + download_extra + "}";
    }

    std::string routine_step() const
    {
        return R"({"step": "routine", "name": "start", "session": )"
               R"("programming", "routine": )" +
               routine + routine_extra + "}";
    }

    std::string json() const
    {
        return R"([{"id": "kernel-read", "kind": )" + kind +
               R"(, "title": "Through the helper", "ecu": "pcm", )"
               R"("level": "01", "kernel": )" +
               kernel() + ", \"steps\": " + steps() + "}]";
    }
};

inline Made made(const Kernel& k)
{
    Made m;
    m.format = "3";
    m.tiers = R"(["data", "conversation", "operation"])";
    m.publisher = kPublisher;
    m.operations = k.json();
    m.files_extra = R"(, "kernel.bin": {"kind": "bin", "base64": ")" +
                    base64(k.blob) + "\"}";
    return m;
}

} // namespace kernel_test

#endif /* ECUIO_TESTS_PLUGIN_KERNEL_SUPPORT_H */
