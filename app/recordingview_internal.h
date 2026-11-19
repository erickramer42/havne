// File/layer: app/recordingview_internal.h | Qt6 Core, app layer, C++17
// Testing: BRIDGE: RecordingViewTest (test_bridge)

#pragma once

#include "recordingview.h"

#include "recording_frameset.h"
#include "urlwrite.h"

#include "healthreport.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QList>
#include <QMetaObject>
#include <QThreadPool>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include "ecuio/conditions.h"
#include "ecuio/core.h"
#include "ecuio/profile.h"

#include "recording_signals.h" /* cli/: events.log's profile and file lines */
#include "view_frames.h"       /* cli/: which set decodes the frames */

// What the viewer's source files share: unit helpers and the open's result.
// Internal: only recordingview*.cpp include it.

namespace recordingview_detail {

// The most columns one query fills: well past any screen's width
constexpr int kMaxColumns = 65536;
// A text_at copy: a text signal's value is cut past this
constexpr uint32_t kTextCapacity = 1024;

inline double toMs(uint64_t us)
{
    return static_cast<double>(us) / 1000.0;
}

// -1 for "none", as QML reads it
inline double toMsOrNone(uint64_t us)
{
    return us == ECUIO_RECORDING_NONE ? -1.0 : toMs(us);
}

// A time from QML: negative and NaN are 0, the rest to the microsecond
inline uint64_t fromMs(double ms)
{
    if (!(ms > 0)) {
        return 0;
    }
    const double us = std::round(ms * 1000.0);
    if (us >= 1.8e19) {
        return std::numeric_limits<uint64_t>::max();
    }
    return static_cast<uint64_t>(us);
}

// A time from QML that may be before the start, to the microsecond; false
// for NaN or past a few hundred years either way
inline bool signedUs(double ms, int64_t* out)
{
    const double us = std::round(ms * 1000.0);
    if (!(std::fabs(us) < 9e15)) {
        return false;
    }
    *out = static_cast<int64_t>(us);
    return true;
}

// Why a folder of values alone (a CSV import) has no check or decode
constexpr const char* kNoFrames = "the recording has no frames";

inline QString detailOr(ecuio_status_t rc)
{
    const char* detail = ecuio_last_error_detail();
    if (detail != nullptr && detail[0] != '\0') {
        return QString::fromUtf8(detail);
    }
    return QString::fromUtf8(ecuio_status_string(rc));
}

} // namespace recordingview_detail

// One entry of conditionSpans, as the open's thread pairs it
struct RecordingView::ConditionSpan {
    uint32_t condition = 0; // its place in Result::conditionKeys
    bool on = false;
    bool heldStart = false;
    bool openEnd = false;
    uint64_t from_us = 0;
    uint64_t to_us = 0;
    std::vector<std::pair<uint64_t, uint64_t>> stale;
};

// What the thread hands the GUI thread. A result dropped unread (a later
// open, the view destroyed) closes its reader.
struct RecordingView::Result {
    ecuio_recording_t* reader = nullptr;
    ecuio_status_t rc = ECUIO_OK;
    QString detail;
    bool checked = false;
    ecuio_status_t checkRc = ECUIO_OK;
    QString checkDetail;
    ecuio_check_summary_t summary{};
    std::vector<ecuio_check_finding_t> findings;
    bool findingsLost = false;
    // Each key the cond lines name: { key, name, level }
    struct ConditionKey {
        std::string key;
        QString name;
        int level = ECUIO_ALERT_NONE;
    };
    std::vector<ConditionKey> conditionKeys;
    std::vector<ConditionSpan> conditionSpans;
    uint64_t conditionSpanCount = 0; // every entry, the ones past the cap too
    bool conditionPartsCut = false;  // stale parts merged past their bound
    bool cutForTesting = false;      // setConditionsCutForTesting's

    Result() = default;
    Result(const Result&) = delete;
    Result& operator=(const Result&) = delete;
    ~Result() { ecuio_recording_close(reader); }
};
