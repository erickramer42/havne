#pragma once

#ifdef _WIN32

#include <windows.h>

#include <filesystem>
#include <string>
#include <system_error>

/* TMP and TEMP name a fresh folder while it lives, so a case reads its own session logs */
class SessionLogDir
{
public:
    SessionLogDir()
    {
        wchar_t base[MAX_PATH + 1] = {};
        GetTempPathW(MAX_PATH + 1, base);
        dir_ = std::filesystem::path(base) /
               (L"havne-sur-log-" + std::to_wstring(GetCurrentProcessId()));
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
        std::filesystem::create_directories(dir_, ec);
        const std::wstring value = dir_.wstring() + L"\\";
        for (Saved& v : saved_) {
            const DWORD n = GetEnvironmentVariableW(v.name, nullptr, 0);
            v.had = n > 0;
            if (v.had) {
                v.value.resize(n);
                GetEnvironmentVariableW(v.name, &v.value[0], n);
                v.value.resize(n - 1);
            }
            SetEnvironmentVariableW(v.name, value.c_str());
        }
    }
    ~SessionLogDir()
    {
        for (const Saved& v : saved_) {
            SetEnvironmentVariableW(v.name, v.had ? v.value.c_str() : nullptr);
        }
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }
    SessionLogDir(const SessionLogDir&) = delete;
    SessionLogDir& operator=(const SessionLogDir&) = delete;

    std::string text() const
    {
        std::string out;
        std::error_code ec;
        for (const auto& e : std::filesystem::directory_iterator(dir_, ec)) {
            const HANDLE f = CreateFileW(
                e.path().c_str(), GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (f == INVALID_HANDLE_VALUE)
                continue;
            char buf[4096];
            DWORD rd = 0;
            while (ReadFile(f, buf, sizeof(buf), &rd, nullptr) && rd > 0) {
                out.append(buf, rd);
            }
            CloseHandle(f);
        }
        return out;
    }

private:
    struct Saved {
        const wchar_t* name;
        bool had;
        std::wstring value;
    };
    std::filesystem::path dir_;
    Saved saved_[2] = {{L"TMP", false, {}}, {L"TEMP", false, {}}};
};

#endif
