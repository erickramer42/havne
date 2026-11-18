// File/layer: core/src/least_core.h | internal, C++17, Qt-free, header-only
// Testing: UNIT: PluginWrapTest (test_signals)
//
// The least core version that reads each thing a signal file or a plugin
// can hold (ADR-0064 Q1): a wrap writes the highest the input uses as its
// requires.core, so the same input wraps to the same bytes under any core.
// One row for each format and each feature an older core refuses or reads
// to another meaning, its release and evidence beside it; the loaders
// raise a file's or a plugin's least at the gate that reads the feature.
//
// A new key, kind, function or raised bound in a loader adds its row here
// and its call at the gate. A row may name the release to come: the
// result is cut to this core's version (within_core), so a build between
// the feature and its release still loads what it writes.

#ifndef ECUIO_LEAST_CORE_H
#define ECUIO_LEAST_CORE_H

#include "ecuio/core.h"
#include "ecuio/plugin.h"
#include "ecuio/profile.h"
#include "ecuio/signals.h"
#include "plugin_rules.h"

#include <cstdint>
#include <string>

namespace ecuio_least {

struct version {
    uint32_t v[3];
};

/* A packed plugin: format 1, its manifest and bounds (v0.5.0, 069b4f0b) */
inline constexpr version kPackage = {{0, 5, 0}};
/* A signal file of format 1 (v0.5.0, ba511e51) */
inline constexpr version kSignalFile = {{0, 5, 0}};
/* A signal's "gauge" (v0.7.2) */
inline constexpr version kGauge = {{0, 7, 2}};
/* An extending file's signal named as a base def replaces it (v0.7.3;
 * v0.7.2 kept both, two defs of one name) */
inline constexpr version kReplacesByName = {{0, 7, 3}};
/* More requests than kRequestsBefore, to ECUIO_PROFILE_MAX_REQUESTS
 * (v0.8.0, b6982228) */
inline constexpr version kRequests = {{0, 8, 0}};
inline constexpr uint32_t kRequestsBefore = 256;
/* Signal-file format 2, the computed grammar (v0.8.0, ADR-0062) */
inline constexpr version kFormat2 = {{0, 8, 0}};
/* A manifest's "lineage" (v0.8.0, ADR-0064 Q1) */
inline constexpr version kLineage = {{0, 8, 0}};
/* A data file's own "source", other than the manifest's, names its
 * variant's (v0.8.0; v0.7.4 took the manifest's) */
inline constexpr version kOwnSource = {{0, 8, 0}};
/* More ECUs than kEcusBefore, the base's counted, to ECUIO_MAX_CHANNELS
 * (v0.9.0, ADR-0065) */
inline constexpr version kEcus = {{0, 9, 0}};
inline constexpr uint32_t kEcusBefore = 4;
/* More requests in all than kSetRequestsBefore, each module's within
 * ECUIO_MODULE_MAX_REQUESTS (v0.9.0, ADR-0065) */
inline constexpr version kSetRequests = {{0, 9, 0}};
inline constexpr uint32_t kSetRequestsBefore = 1024;
/* A variant's files' signals together past one file's
 * ECUIO_SIGNAL_FILE_MAX_SIGNALS (v0.9.0, ADR-0065) */
inline constexpr version kSetSignals = {{0, 9, 0}};
/* A variant's written copy past one file's ECUIO_SIGNAL_FILE_MAX_BYTES
 * (v0.9.0, ADR-0065) */
inline constexpr version kSetBytes = {{0, 9, 0}};

/* Each "havne_signals" by its number: [0] stands for none */
inline constexpr version kSignalFormatSince[] = {kSignalFile, kSignalFile,
                                                 kFormat2};
static_assert(sizeof kSignalFormatSince / sizeof kSignalFormatSince[0] ==
                  ECUIO_SIGNAL_FILE_FORMAT + 1,
              "a new signal-file format needs its row in kSignalFormatSince");

/* Each "havne_plugin" by its number: [0] stands for none */
/* A plugin of format 2 (ADR-0066): the conversation tier, its keys and a
 * binary file. An older core refuses the format whole. */
inline constexpr version kPluginFormat2 = {{0, 9, 0}};

inline constexpr version kPluginFormatSince[] = {kPackage, kPackage,
                                                 kPluginFormat2};
static_assert(sizeof kPluginFormatSince / sizeof kPluginFormatSince[0] ==
                  ECUIO_PLUGIN_FORMAT_MAX + 1,
              "a new plugin format needs its row in kPluginFormatSince");

/* The bounds an older core refuses past, as the rows know them: a
 * raised one needs a row above and its call at the loader's gate */
static_assert(ECUIO_MAX_CHANNELS == 32,
              "ECUIO_MAX_CHANNELS moved: add a row beside kEcus");
static_assert(ECUIO_PROFILE_MAX_REQUESTS == 1024,
              "ECUIO_PROFILE_MAX_REQUESTS moved: add a row beside kRequests");
static_assert(ECUIO_SIGNAL_FILE_MAX_SIGNALS == 1024,
              "ECUIO_SIGNAL_FILE_MAX_SIGNALS moved: add a row for it");
static_assert(ECUIO_SIGNAL_FILE_MAX_BYTES == 1048576,
              "ECUIO_SIGNAL_FILE_MAX_BYTES moved: add a row for it");
static_assert(ECUIO_MODULE_MAX_REQUESTS == 1024 &&
                  ECUIO_SIGNAL_SET_MAX_SIGNALS == 16384 &&
                  ECUIO_SIGNAL_SET_MAX_BYTES == 4194304,
              "a set's bound moved: add a row beside kSetRequests");
static_assert(ECUIO_COMPUTED_MAX_INPUTS == 4 &&
                  ECUIO_COMPUTED_MAX_CONSTANTS == 16 &&
                  ECUIO_COMPUTED_MAX_STEPS == 33 &&
                  ECUIO_COMPUTED_MAX_DEPTH == 8,
              "a computed bound moved: add a row for it beside kFormat2");
static_assert(ECUIO_PLUGIN_PACKAGE_MAX == 16777216 &&
                  ECUIO_PLUGIN_MAX_FILES == 64 &&
                  ECUIO_PLUGIN_MAX_VARIANTS == 32 &&
                  ECUIO_PLUGIN_MAX_FIXTURES == 16,
              "a plugin bound moved: add a row for it beside kPackage");
static_assert(ECUIO_ENUM_MAX_ENTRIES == 256 && ECUIO_ENUM_LABEL_MAX == 27 &&
                  ECUIO_SET_MAX_TABLE == 65536,
              "an enum or table bound moved: add a row for it");
static_assert(ECUIO_FRAME_DATA_MAX == 4096,
              "ECUIO_FRAME_DATA_MAX moved (a field's byte): add a row for it");
static_assert(ECUIO_COMPUTED_MAX_AGE_US == 60000000,
              "ECUIO_COMPUTED_MAX_AGE_US moved (a max_age): add a row for it");
/* Bounded from v0.8.0 (an older core takes any length) */
static_assert(ECUIO_SOURCE_TEXT_MAX == 512,
              "ECUIO_SOURCE_TEXT_MAX moved: add a row for it");
static_assert(ECUIO_PROFILE_NAME_MAX == 63,
              "ECUIO_PROFILE_NAME_MAX moved (a name's profile): add a row");
static_assert(ECUIO_PLUGIN_ID_MAX == 31 && ECUIO_PLUGIN_VARIANT_ID_MAX == 24 &&
                  ECUIO_PLUGIN_PATH_MAX == 127,
              "a plugin id or path bound moved: add a row beside kPackage");

inline bool older(const version& a, const version& b)
{
    return ecuio_plugin_rules::older(a.v, b.v);
}

/* at raised to need when need is later */
inline void raise(version& at, const version& need)
{
    if (older(at, need)) {
        at = need;
    }
}

/* v, or this core's version when v is later: a row of a release to come */
inline version within_core(const version& v)
{
    version core = {{0, 0, 0}};
    ecuio_plugin_rules::core_version(core.v);
    return older(core, v) ? core : v;
}

inline std::string text(const version& v)
{
    return std::to_string(v.v[0]) + "." + std::to_string(v.v[1]) + "." +
           std::to_string(v.v[2]);
}

/* The least core that reads a loaded plugin (plugin.cpp) */
version of_plugin(const ecuio_plugin_t* plugin);

} // namespace ecuio_least

#endif /* ECUIO_LEAST_CORE_H */
