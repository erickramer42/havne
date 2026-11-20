// File/layer: core/src/obdb_import_words.h | internal, not ABI-exported; included by obdb_importer.h and obdb_import.cpp
// C++17, Qt-free
// Testing: UNIT: ObdbImportTest (test_core); FUZZ: fuzz_obdb_import
//
// The OBDb import's words and rules (obdb_import.cpp): the exceptions, the
// unit table, text cleaning, hex and key spelling.

#ifndef ECUIO_OBDB_IMPORT_WORDS_H
#define ECUIO_OBDB_IMPORT_WORDS_H

#include "json.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <new>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace ecuio_obdb_impl {

using ecuio_json::Member;
using ecuio_json::number_text;
using ecuio_json::Type;
using ecuio_json::Value;

/* The caller's note callback asked to stop */
struct canceled {};

/* Input that isn't an OBDb signal set at all */
struct not_obdb {
    uint32_t line;
    uint32_t column;
    std::string words;
};

/* OBDb's unit words and the spelling the built-in sets give each, so a
 * converted unit converts where theirs do (ADR-0022) */
struct unit_word {
    const char* obdb;
    const char* havne;
};
const unit_word kUnits[] = {
    {"ampereHours", "Ah"},
    {"amps", "A"},
    {"bars", "bar"},
    {"celsius", "\xC2\xB0"
                "C"},
    {"centimeters", "cm"},
    {"coulombs", "C"},
    {"degrees", "\xC2\xB0"},
    {"fahrenheit", "\xC2\xB0"
                   "F"},
    {"feet", "ft"},
    {"framesPerSecond", "fps"},
    {"gallons", "gal"},
    {"gallonsPerHour", "gal/h"},
    {"gigahertz", "GHz"},
    {"gramsPerLiter", "g/L"},
    {"gramsPerSecond", "g/s"},
    {"gravity", "g"},
    {"hertz", "Hz"},
    {"hours", "h"},
    {"inches", "in"},
    {"inchPound", "in\xC2\xB7lb"},
    {"joules", "J"},
    {"kelvin", "K"},
    {"kiloampereHours", "kAh"},
    {"kiloamps", "kA"},
    {"kilogramsPerHour", "kg/h"},
    {"kilohertz", "kHz"},
    {"kilojoules", "kJ"},
    {"kilometers", "km"},
    {"kilometersPerHour", "km/h"},
    {"kiloohms", "k\xCE\xA9"},
    {"kilopascal", "kPa"},
    {"kilovolts", "kV"},
    {"kilowattHours", "kWh"},
    {"kilowattHoursPer100Kilometers", "kWh/100km"},
    {"kilowattHoursPer100Miles", "kWh/100mi"},
    {"kilowatts", "kW"},
    {"liters", "L"},
    {"litersPerHour", "L/h"},
    {"megahertz", "MHz"},
    {"megaohms", "M\xCE\xA9"},
    {"meters", "m"},
    {"metersPerSecond", "m/s"},
    {"metersPerSecondSquared", "m/s\xC2\xB2"},
    {"microhertz", "\xC2\xB5Hz"},
    {"microohms", "\xC2\xB5\xCE\xA9"},
    {"miles", "mi"},
    {"milesPerHour", "mph"},
    {"milesPerKilowattHour", "mi/kWh"},
    {"milliampereHours", "mAh"},
    {"milliamps", "mA"},
    {"milligramsPerDeciliter", "mg/dL"},
    {"milligramsPerStroke", "mg/stroke"},
    {"millihertz", "mHz"},
    {"millimeters", "mm"},
    {"milliohms", "m\xCE\xA9"},
    {"milliseconds", "ms"},
    {"millivolts", "mV"},
    {"milliwatts", "mW"},
    {"minutes", "min"},
    {"nanohertz", "nHz"},
    {"newtonMeters", "N\xC2\xB7m"},
    {"ohms", "\xCE\xA9"},
    {"percent", "%"},
    {"poundFoot", "lb\xC2\xB7"
                  "ft"},
    {"psi", "psi"},
    {"radians", "rad"},
    {"rpm", "RPM"},
    {"seconds", "s"},
    {"terahertz", "THz"},
    {"volts", "V"},
    {"watts", "W"},
    {"yards", "yd"},
    {"scalar", ""},
    {"unknown", ""},
};

/* The first part of an OBDb path and the signal file group it falls in;
 * a path not here is Other */
struct path_group {
    const char* path;
    const char* group;
};
const path_group kGroups[] = {
    {"Engine", "engine"},         {"Fluids", "engine"},
    {"Control", "air"},           {"Fuel", "fuel"},
    {"Emissions", "emissions"},   {"Battery", "electrical"},
    {"Electrical", "electrical"}, {"Movement", "vehicle"},
    {"Drivetrain", "vehicle"},    {"Transmission", "vehicle"},
    {"Tires", "vehicle"},         {"Brakes", "vehicle"},
    {"Suspension", "vehicle"},    {"Trips", "vehicle"},
    {"Stats", "vehicle"},         {"Cruise control", "vehicle"},
    {"DTCs", "diagnostics"},      {"Monitors", "diagnostics"},
    {"ECU", "diagnostics"},       {"OBD", "diagnostics"},
    {"Details", "diagnostics"},
};

/* A JSON string of text with no control characters (clean() made it so) */
inline void add_string(std::string& out, const std::string& s)
{
    out += '"';
    for (const char c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
        }
        out += c;
    }
    out += '"';
}

/* Text with every control character a space, and every character
 * ecuio_json::invisible_length finds, cut to max bytes at a whole
 * character; *cut says whether it was cut */
inline std::string clean(const std::string& s, size_t max, bool* cut)
{
    std::string t;
    t.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        const auto c = static_cast<unsigned char>(s[i]);
        const size_t hidden =
            ecuio_json::invisible_length(s.data() + i, s.size() - i);
        if (c < 0x20 || c == 0x7F || hidden > 0) {
            t += ' ';
            i += hidden > 0 ? hidden : 1;
        } else {
            t += s[i];
            ++i;
        }
    }
    *cut = t.size() > max;
    if (*cut) {
        size_t n = max;
        while (n > 0 && (static_cast<unsigned char>(t[n]) & 0xC0) == 0x80) {
            --n;
        }
        t.resize(n);
    }
    return t;
}

/* Text a spreadsheet won't run as a formula (ADR-0049 section 8): its
 * leading '=', '+', '-' and '@' dropped, with the spaces between them,
 * so what's left starts with neither; true when any was */
inline bool unformula(std::string* s)
{
    size_t n = 0;
    bool any = false;
    while (
        n < s->size() && (*s)[n] != '\0' &&
        (std::strchr("=+-@", (*s)[n]) != nullptr || (any && (*s)[n] == ' '))) {
        any = any || (*s)[n] != ' ';
        ++n;
    }
    s->erase(0, n);
    return any;
}

/* Text without its leading and trailing spaces */
inline std::string trimmed(const std::string& s)
{
    const size_t a = s.find_first_not_of(' ');
    if (a == std::string::npos) {
        return std::string();
    }
    return s.substr(a, s.find_last_not_of(' ') - a + 1);
}

/* Hex digits, 1 to max of them, as a value */
inline bool hex_of(const std::string& s, size_t max, uint32_t* out)
{
    if (s.empty() || s.size() > max) {
        return false;
    }
    uint32_t v = 0;
    for (const char c : s) {
        uint32_t d = 0;
        if (c >= '0' && c <= '9') {
            d = static_cast<uint32_t>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            d = static_cast<uint32_t>(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            d = static_cast<uint32_t>(c - 'A' + 10);
        } else {
            return false;
        }
        v = v * 16 + d;
    }
    *out = v;
    return true;
}

inline std::string hex_text(uint32_t v, int digits)
{
    char buf[16];
    std::snprintf(buf, sizeof buf, "%0*X", digits, v);
    return buf;
}

/* A string member, or "" */
inline std::string text_at(const Value& obj, const char* key)
{
    const Value* v = obj.find(key);
    return (v != nullptr && v->type == Type::string) ? v->text : std::string();
}

/* The fewest decimals, 0 to 3, that show each step of a scale and offset */
inline uint32_t decimals_of(double scale, double offset)
{
    uint32_t d = 0;
    for (const double v : {scale, offset}) {
        uint32_t k = 0;
        double x = v;
        while (k < 3 && std::fabs(x - std::round(x)) >
                            1e-9 * std::max(1.0, std::fabs(x))) {
            ++k;
            x *= 10;
        }
        d = std::max(d, k);
    }
    return d;
}

/* A key of [a-z0-9_], 1 to 31 bytes, from an OBDb id: camel case split,
 * a letter first (an expression reads a leading digit as a number) */
inline std::string key_base(const std::string& id)
{
    std::string k;
    char prev = '_';
    for (const char c : id) {
        char out = '_';
        if (c >= 'A' && c <= 'Z') {
            if (prev >= 'a' && prev <= 'z') {
                k += '_';
            }
            out = static_cast<char>(c - 'A' + 'a');
        } else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            out = c;
        }
        if (out == '_' && (k.empty() || k.back() == '_')) {
            prev = c;
            continue;
        }
        k += out;
        prev = c;
    }
    while (!k.empty() && k.back() == '_') {
        k.pop_back();
    }
    if (k.empty() || (k[0] >= '0' && k[0] <= '9')) {
        k = "s_" + k;
    }
    if (k.size() > 31) {
        k.resize(31);
        while (k.back() == '_') {
            k.pop_back();
        }
    }
    return k;
}

/* A key not yet taken: the base, else the base cut to fit "_2", "_3"... */
inline std::string unique_key(const std::string& base, std::set<std::string>& taken)
{
    std::string k = base;
    for (unsigned n = 2; taken.count(k) != 0; ++n) {
        const std::string tail = "_" + std::to_string(n);
        k = base.substr(0, 31 - tail.size()) + tail;
    }
    taken.insert(k);
    return k;
}

/* A name not yet taken: the name, else cut to fit " 2", " 3"... at a whole
 * character, as clean() cuts. A set's names are one each (a recording keys
 * values by name), and a file's signal named as a base def would replace
 * it, which a converted set never means to. */
inline std::string unique_name(const std::string& base, std::set<std::string>& taken,
                        bool* repeated)
{
    std::string n = base;
    *repeated = false;
    for (unsigned k = 2; taken.count(n) != 0; ++k) {
        const std::string tail = " " + std::to_string(k);
        size_t cut = 31 - tail.size();
        while (cut > 0 && cut < base.size() &&
               (static_cast<unsigned char>(base[cut]) & 0xC0) == 0x80) {
            --cut;
        }
        n = base.substr(0, cut) + tail;
        *repeated = true;
    }
    taken.insert(n);
    return n;
}

} // namespace ecuio_obdb_impl

#endif /* ECUIO_OBDB_IMPORT_WORDS_H */
