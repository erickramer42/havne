// File/layer: tests/partial_left.h | test-only, C++17, Qt-free
// Testing: the helper itself; ExportTest, FolderExportTest, CsvExportTest,
//          ImportTest, CsvImportTest, HealthTest (test_core)
//
// Whether a file or folder that core writes under a name of its own and
// then renames into place (folder_files.h's partial_name:
// <name>.partial-<pid>-<n>) was left beside it.

#ifndef ECUIO_TESTS_PARTIAL_LEFT_H
#define ECUIO_TESTS_PARTIAL_LEFT_H

#include <filesystem>
#include <string>

/* Any entry of dir named <name>.partial-...: a call's own partial left
 * behind (another's, named <name>.partial alone, isn't counted) */
inline bool partial_left(const std::filesystem::path& dir,
                         const std::string& name)
{
    const std::string prefix = name + ".partial-";
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        if (e.path().filename().generic_u8string().rfind(prefix, 0) == 0) {
            return true;
        }
    }
    return false;
}

#endif /* ECUIO_TESTS_PARTIAL_LEFT_H */
