// File/layer: app/healthreport.h | Qt6 Core, app layer, C++17
// Testing: BRIDGE: BridgeTest, RecordingViewTest (test_bridge)
//
// The health report (ADR-0054's appendix "the health report as built")
// written to the file the user picked, for Diagnostics' and the log
// viewer's Save report. Core writes only to a path and refuses one that
// exists, while the picker has already asked about replacing a file and
// on Android hands back a content:// URI. So core writes into a temporary
// folder of the app's own, and the bytes its check passed go to the
// picked file through writeToUrl. Callable from any thread.

#pragma once

#include <QByteArray>
#include <QString>
#include <QUrl>
#include <QVariantMap>

#include <atomic>
#include <cstdint>

#include "ecuio/recording.h"

// The format the picked file takes: JSON for the dialog's second filter
// (filterIndex 1) or a name ending .json, text otherwise. Android's
// content:// URI carries no name, so the filter decides there
uint32_t healthFormatFor(const QUrl& url, int filterIndex);

// The report's time (now, microseconds since 1970 UTC) and the latest
// model year a VIN's decode takes (this year + 1, as the front ends pass)
uint64_t healthUtcNow();
uint32_t healthLatestModelYear();

// A baseline's folder as core takes it (UTF-8, the platform's separators),
// empty for none: baseline_folder_utf8 is then NULL
QByteArray healthBaselineArgument(const QString& folder);

// A written report's counts in words, each noun as its count wants it:
// "1 ECU, 3 trouble codes, 0 key values"; and the log's line, "Health
// report saved: <summary> (2 identification items left out)"
QString healthSummary(const ecuio_health_report_t& report);
QString healthSavedLine(const ecuio_health_report_t& report);

// options: every field but out_path_utf8, which this sets. The result:
// { ok, words, ecus, codes, values, sinceCleared, identificationLeftOut,
// vinDecoded, baselineRead, summary, line }, baselineRead whether core
// read the baseline named (the report says why not), words "" on success
// and never a path, a
// folder's name or a value; summary healthSummary's ("" on failure);
// line the log's, in the same words. stop, when given, ends a report
// from a recording's folder at core's next ask (ecuio_health_export_with),
// with nothing written. A report that isn't written removes the document a
// picker made at url when it is a provider's (not a local file, or
// throughProvider, which a test sets)
QVariantMap writeHealthReport(ecuio_health_options_t options, const QUrl& url,
                              std::atomic<bool>* stop = nullptr,
                              bool throughProvider = false);
