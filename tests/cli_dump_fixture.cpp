#include "operation_plan_support.h"
#include "plugin_kernel_support.h"
#include "sim_kernel.h"

#include <cstdio>
#include <string>

namespace {

bool write_text(const char* path, const std::string& text)
{
    FILE* f = std::fopen(path, "wb");
    if (f == nullptr) {
        return false;
    }
    const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    return std::fclose(f) == 0 && ok;
}

std::string kernel_package(const char* bytes = nullptr,
                           const char* functional = nullptr)
{
    kernel_test::Kernel k;
    if (bytes != nullptr) {
        k.ranges =
            R"({"step": "upload", "name": "code", "session": "programming", )"
            R"("address": "00080000", "bytes": )" +
            std::string(bytes) +
            R"(, "address_bytes": 4, "size_bytes": 4, "dfi": "00"})";
    }
    if (functional != nullptr) {
        k.functional = functional;
    }
    k.blob.resize(2000);
    ecuio_sim_kernel_image(k.blob.data(), 2000);
    conversation_test::Made m = session_operation::made();
    m.operations = k.json();
    m.files_extra = R"(, "kernel.bin": {"kind": "bin", "base64": ")" +
                    conversation_test::base64(k.blob) + "\"}";
    return m.json();
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 3 && argc != 5) {
        std::fprintf(stderr, "usage: cli_dump_fixture <plain.json> "
                             "<kernel.json> [<long.json> <bare.json>]\n");
        return 2;
    }
    const std::string plain =
        session_operation::made({}, session_operation::one_frame_plan()).json();
    bool ok =
        write_text(argv[1], plain) && write_text(argv[2], kernel_package());
    if (argc == 5) {
        ok = ok && write_text(argv[3], kernel_package("16384")) &&
             write_text(argv[4],
                        kernel_package("16384", R"([{"request": "10 82"}])"));
    }
    if (!ok) {
        std::fprintf(stderr, "cli_dump_fixture: a file didn't write\n");
        return 1;
    }
    return 0;
}
