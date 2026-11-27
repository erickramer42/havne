#ifndef ECUIO_HOST_POWER_H
#define ECUIO_HOST_POWER_H

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace host_power {

constexpr int kWriteLeastPercent = 30;

struct reading {
    bool known = false;
    bool mains = true;
    int percent = -1; /* the battery's, -1 for none */
};

/* One of Linux's /sys/class/power_supply entries */
struct supply {
    const char* type = ""; /* "Mains", "Battery", "USB", ... */
    int online = -1;       /* -1 unread */
    int capacity = -1;
};

/* Any mains online is mains; with none, a battery says how full. A host
 * listing no battery is on mains (a desktop); one listing nothing at all
 * is unknown. */
inline reading judge_supplies(const supply* s, size_t n)
{
    reading r;
    bool battery = false;
    bool mains_online = false;
    bool mains_listed = false;
    for (size_t i = 0; i < n; ++i) {
        if (std::strcmp(s[i].type, "Mains") == 0 ||
            std::strcmp(s[i].type, "USB") == 0) {
            mains_listed = true;
            mains_online = mains_online || s[i].online == 1;
        } else if (std::strcmp(s[i].type, "Battery") == 0) {
            battery = true;
            if (s[i].capacity >= 0 &&
                (r.percent < 0 || s[i].capacity < r.percent)) {
                r.percent = s[i].capacity > 100 ? 100 : s[i].capacity;
            }
        }
    }
    if (!battery && !mains_listed) {
        return reading{};
    }
    r.known = true;
    r.mains = mains_online || !battery;
    return r;
}

/* A write off mains under kWriteLeastPercent stops; any run off mains is
 * told so */
inline bool write_stops(const reading& r)
{
    return r.known && !r.mains && r.percent >= 0 &&
           r.percent < kWriteLeastPercent;
}

inline bool off_mains(const reading& r)
{
    return r.known && !r.mains;
}

/* The platform's (host_power.cpp): Windows' power status, Linux's
 * supplies; unknown elsewhere */
reading read();

} // namespace host_power

#endif /* ECUIO_HOST_POWER_H */
