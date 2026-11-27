#include "host_power.h"

#include "ecuio/internal.h"

#include <atomic>

#ifdef _WIN32
#include <windows.h>
#elif defined(__linux__) && !defined(__ANDROID__)
#include <dirent.h>

#include <cstdio>
#include <string>
#include <vector>
#endif

namespace {

std::atomic<ecuio_test_host_power_t> g_seam{nullptr};

#ifdef _WIN32
host_power::reading platform()
{
    SYSTEM_POWER_STATUS st{};
    host_power::reading r;
    if (!GetSystemPowerStatus(&st) || st.ACLineStatus == 255) {
        return r;
    }
    r.known = true;
    r.mains = st.ACLineStatus == 1 || (st.BatteryFlag & 128) != 0;
    r.percent = st.BatteryLifePercent == 255 ? -1 : st.BatteryLifePercent;
    return r;
}
#elif defined(__linux__) && !defined(__ANDROID__)
int read_int(const std::string& path)
{
    FILE* f = std::fopen(path.c_str(), "r");
    if (f == nullptr) {
        return -1;
    }
    int v = -1;
    if (std::fscanf(f, "%d", &v) != 1) {
        v = -1;
    }
    std::fclose(f);
    return v;
}

std::string read_word(const std::string& path)
{
    char buf[32] = {};
    FILE* f = std::fopen(path.c_str(), "r");
    if (f == nullptr) {
        return {};
    }
    if (std::fscanf(f, "%31s", buf) != 1) {
        buf[0] = '\0';
    }
    std::fclose(f);
    return buf;
}

host_power::reading platform()
{
    const std::string root = "/sys/class/power_supply/";
    DIR* d = opendir(root.c_str());
    if (d == nullptr) {
        return {};
    }
    std::vector<std::string> types;
    std::vector<host_power::supply> found;
    for (dirent* e = readdir(d); e != nullptr && found.size() < 16;
         e = readdir(d)) {
        if (e->d_name[0] == '.') {
            continue;
        }
        const std::string at = root + e->d_name + "/";
        types.push_back(read_word(at + "type"));
        host_power::supply s;
        s.online = read_int(at + "online");
        s.capacity = read_int(at + "capacity");
        found.push_back(s);
    }
    closedir(d);
    for (size_t i = 0; i < found.size(); ++i) {
        found[i].type = types[i].c_str();
    }
    return host_power::judge_supplies(found.data(), found.size());
}
#else
host_power::reading platform()
{
    return {};
}
#endif

} // namespace

host_power::reading host_power::read()
{
    if (const ecuio_test_host_power_t seam = g_seam.load()) {
        int percent = -1;
        const int state = seam(&percent);
        reading r;
        r.known = state >= 0;
        r.mains = state != 0;
        r.percent = percent;
        return r;
    }
    try {
        return platform();
    } catch (...) {
        return {};
    }
}

extern "C" void ecuio_test_set_host_power(ecuio_test_host_power_t read)
{
    g_seam.store(read);
}
