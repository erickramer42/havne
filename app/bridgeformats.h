// File/layer: app/bridgeformats.h | Qt6 Core, app layer, C++17
// Testing: BRIDGE: BridgeTest (test_bridge)

#pragma once

#include <QByteArray>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

#include <cstdint>
#include <utility>
#include <vector>

#include "ecuio/core.h"
#include "ecuio/profile.h"
#include "ecuio/signals.h"

// SessionBridge's stateless statics: text and map builders that take their
// inputs as arguments, so a test needs no session. Public, and named through
// SessionBridge as before.
struct BridgeFormats
{
    // The profile a session on uri decodes through, as core picks it at
    // ecuio_session_create: the last profile= (1 to
    // ECUIO_PROFILE_NAME_MAX bytes), else the scheme's default. "" for
    // a scheme this platform's core doesn't take, a malformed query or
    // a bad profile= value. Static, so tests need no session.
    static QString profileOfUri(const QString& uri);
    static QString linkStateForError(int32_t errorCode);  /* code -> state string; static so tests need no session */
    static QString sessionInfoText(const ecuio_session_info_t& info);  /* the activity log's "Session:" line */
    static QString versionsText(const ecuio_session_info_t& info);  /* its versions clause; "" with none read */
    static QString channelSupportText(const ecuio_channel_support_t& support);  /* its "ECU 0x…:" lines */
    // A module's session change (ADR-0060): "ECU 0x7E8: reading in the
    // extended session (50 03)" and the like; "" for none to tell
    static QString channelSessionText(const ecuio_channel_support_t& support);
    // At Disconnect, for a module that entered the extended session; ""
    // otherwise
    static QString sessionReturnedText(const ecuio_channel_support_t& support);
    // Live's "Refused: <name> (7F <service> <code>)"
    static QString refusalText(uint8_t service, uint8_t nrc);
    // The negative reply alone: "7F 22 31"
    static QString refusalCode(uint8_t service, uint8_t nrc);
    // refusedUnnamed's words for each ECU's ID and its refusals past the list
    static QString
    refusedUnnamedText(const std::vector<std::pair<uint32_t, uint32_t>>& more);
    static QString channelIdentityText(const ecuio_channel_identity_t& identity);  /* its "ECU 0x… identity:" lines */
    static QString channelDiagnosticsText(const ecuio_channel_diagnostics_t& diagnostics);  /* its "ECU 0x… diagnostics:" lines */
    // its "ECU 0x... clear:" lines (ADR-0038), speeds in the unit system
    static QString channelClearText(const ecuio_channel_diagnostics_t& diagnostics,
                                    uint32_t units);
    // A clear's saved reports, as havne-cli writes them (ADR-0031 section 3): a
    // start line, then one diagnostics line per report
    static QByteArray diagnosticsFileText(const ecuio_channel_diagnostics_t* reports,
                                          uint32_t count);
    /* The activity log's "Reply timing:" line at a disconnect (ADR-0042);
     * empty when no request was asked */
    static QString timingText(const ecuio_session_timing_t& timing,
                              const std::vector<ecuio_request_timing_t>& rows);
    // A recording's replay:// URI, decoding under the profile it was
    // recorded with: the recorded URI's profile= (with its did= under
    // uds-generic), else its scheme's default (mock:// mock, replay://
    // raw, an adapter obd2); raw when nothing was recorded. Static, so
    // tests need no files.
    static QString replayUriFor(const QString& folder,
                                const QString& recordedUri);
    static QString vinSummary(const QString& vin, int latestModelYear);
    // A label as a recording folder takes it: letters, digits, space,
    // -, _ and ., trimmed, at most 40 characters, no trailing dot
    static QString folderLabel(const QString& label);
    // Each cell's first ref the set answers to (a key def's "id:<n>" too),
    // in the cells' order, each once; file is the set's (fileOfSet)
    static std::vector<uint16_t> gaugeIds(const ecuio_signal_set_t* set,
                                          const ecuio_signal_file_t* file,
                                          const QVariantList& cells);
    // A report a clear goes to: its read complete, no clear under way
    static bool clearTarget(const ecuio_channel_diagnostics_t& report);
    // ecuio_diag_inspect's verdict over reports for modelYear (0: none
    // known) as the page shows it: verdict, havne-cli's word ("ready",
    // "not_ready", "mil_on", "year_needed", "not_covered"), "" while a
    // read is under way or when no ECU answered PID 01; text, one
    // sentence naming the incomplete monitors; rule, the rule and its
    // reach. Static, so test_bridge pins it without a session.
    static QVariantMap
    inspectionEntry(const ecuio_channel_diagnostics_t* reports, uint32_t count,
                    uint32_t modelYear);
    // A report that answered there is nothing to erase: PID 01 with the
    // light off and no confirmed codes, the stored and pending lists
    // answered empty, no freeze frame. A refused or unanswered item is
    // not known to be empty, so it never counts as nothing.
    static bool nothingStored(const ecuio_channel_diagnostics_t& report);
    // One ECU's report as the page shows it: ecu ("0x7E8"); asked,
    // complete; readiness (an outcome: "unasked", "answered", "refused",
    // "unanswered") and readinessNrc ("7F 01 12" when refused); mil,
    // dtcCount, compression; lists, three of {key ("stored", "pending",
    // "permanent"), outcome, nrc, codes (["P0301", ...]), more (codes past
    // the held ones)}; cycle, PID 41's outcome (those above, or
    // "unsupported": the ECU's bitmaps leave it out, so it wasn't asked);
    // monitors, [{id, name, complete, cycleEnabled, cycleComplete}], the
    // last two this drive cycle's (PID 41) once cycle is "answered";
    // freeze, {outcome, nrc, stored, code, values: [{name, value, unit,
    // decimals}]}, each value through the obd2 def that decoded it and the
    // unit system; clearing, and the last clear's clear ("none", "cleared",
    // "refused", "unanswered", "engineRunning", "noEngineSpeed",
    // "reportChanged"), clearNrc ("7F 04 22" when refused, "7F 01 12" when
    // the engine speed was) and clearRpm. Static, so test_bridge pins it
    // without a session.
    static QVariantMap diagnosticsEntry(const ecuio_channel_diagnostics_t& d,
                                        uint32_t units);
};
