#ifndef ECUIO_TESTS_PLUGIN_FLASH_SUPPORT_H
#define ECUIO_TESTS_PLUGIN_FLASH_SUPPORT_H

#include "plugin_kernel_support.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace flash_test {

using conversation_test::base64;
using conversation_test::bytes;
using conversation_test::Made;
using conversation_test::sha_of;

inline bytes image(size_t n = 512, uint8_t salt = 0)
{
    bytes b(n);
    for (size_t i = 0; i < n; ++i) {
        b[i] = static_cast<uint8_t>(i * 13 + 5 + salt);
    }
    return b;
}

struct Image {
    std::string name = "code";
    std::string address = "00080000";
    std::string file = "code.bin";
    std::string extra;
    bytes blob = image();
    std::string sha;

    std::string json() const
    {
        return R"({"step": "download", "name": ")" + name +
               R"(", "session": "programming", "address": ")" + address +
               R"(", "address_bytes": 4, "size_bytes": 4, "file": ")" + file +
               R"(", "sha256": ")" + (sha.empty() ? sha_of(blob) : sha) + "\"" +
               extra + "}";
    }
};

/* A flash's parts as text, each a case's to change */
struct Flash {
    std::vector<Image> images = {Image()};
    std::string erase = "\"FF00\"";
    std::string check = "\"FF01\"";
    std::string pass = "\"00\"";
    std::string sectors = R"([{"address": "00080000", "bytes": 256}, )"
                          R"({"address": "00080100", "bytes": 256}])";
    std::string flash_extra;
    std::string kind = "\"flash\"";
    std::string op_extra = R"(, "leave": "reset")";

    std::string flash() const
    {
        return "{\"erase\": " + erase + ", \"check\": " + check +
               ", \"pass\": " + pass + ", \"sectors\": " + sectors +
               flash_extra + "}";
    }

    std::string json() const
    {
        std::string steps;
        for (const Image& i : images) {
            steps += (steps.empty() ? "" : ", ") + i.json();
        }
        return R"([{"id": "flash-code", "kind": )" + kind +
               R"(, "title": "The code", "ecu": "pcm", "level": "01", )"
               R"("flash": )" +
               flash() + ", \"steps\": [" + steps + "]" + op_extra + "}]";
    }
};

inline Made made(const Flash& f)
{
    Made m;
    m.format = "3";
    m.tiers = R"(["data", "conversation", "operation"])";
    m.publisher = kernel_test::kPublisher;
    m.operations = f.json();
    for (const Image& i : f.images) {
        m.files_extra += ", \"" + i.file + R"(": {"kind": "bin", "base64": ")" +
                         base64(i.blob) + "\"}";
    }
    return m;
}

} // namespace flash_test

#endif /* ECUIO_TESTS_PLUGIN_FLASH_SUPPORT_H */
