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

std::string kernel_package()
{
    kernel_test::Kernel k;
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
    if (argc != 3) {
        std::fprintf(stderr, "usage: cli_dump_fixture <plain.json> "
                             "<kernel.json>\n");
        return 2;
    }
    const std::string plain =
        session_operation::made({}, session_operation::one_frame_plan()).json();
    if (!write_text(argv[1], plain) || !write_text(argv[2], kernel_package())) {
        std::fprintf(stderr, "cli_dump_fixture: a file didn't write\n");
        return 1;
    }
    return 0;
}
