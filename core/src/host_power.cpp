#include "host_power.h"

#include "ecuio/internal.h"

#include <atomic>

#ifdef _WIN32
#include <windows.h>
#elif defined(__linux__) && !defined(__ANDROID__)
#include <dirent.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
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
/* The first word of a file of the supply's, into out; "" for none */
void read_field(const char* dir, const char* name, char* out, size_t cap)
{
    char path[320];
    out[0] = '\0';
    if (std::snprintf(path, sizeof path, "/sys/class/power_supply/%s/%s", dir,
                      name) >= static_cast<int>(sizeof path)) {
        return;
    }
    FILE* f = std::fopen(path, "r");
    if (f == nullptr) {
        return;
    }
    if (std::fgets(out, static_cast<int>(cap), f) == nullptr) {
        out[0] = '\0';
    }
    std::fclose(f);
    out[std::strcspn(out, " \n")] = '\0';
}

int read_number(const char* dir, const char* name)
{
    char text[16];
    read_field(dir, name, text, sizeof text);
    return text[0] >= '0' && text[0] <= '9' ? std::atoi(text) : -1;
}

/* Nothing here allocates: a start's out-of-memory pin fails every
 * allocation it makes */
host_power::reading platform()
{
    DIR* d = opendir("/sys/class/power_supply");
    if (d == nullptr) {
        return {};
    }
    constexpr size_t kMost = 16;
    char types[kMost][16];
    host_power::supply found[kMost];
    size_t n = 0;
    for (dirent* e = readdir(d); e != nullptr && n < kMost; e = readdir(d)) {
        if (e->d_name[0] == '.') {
            continue;
        }
        read_field(e->d_name, "type", types[n], sizeof types[n]);
        found[n].type = types[n];
        found[n].online = read_number(e->d_name, "online");
        found[n].capacity = read_number(e->d_name, "capacity");
        ++n;
    }
    closedir(d);
    return host_power::judge_supplies(found, n);
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
    return platform();
}

extern "C" void ecuio_test_set_host_power(ecuio_test_host_power_t read)
{
    g_seam.store(read);
}
