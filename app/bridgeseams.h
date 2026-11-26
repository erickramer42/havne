// File/layer: app/bridgeseams.h | Qt6 Core, app layer, C++17
// Testing: BRIDGE: BridgeTest (test_bridge)

#pragma once

#include <QString>

#include <cstdint>
#include <functional>
#include <utility>

#include "bridgestate.h"

// SessionBridge's test seams that touch only its data members. App code
// never calls one. The ones that emit or call the bridge's own members
// stay in the class.
struct BridgeSeams : protected BridgeState
{
    // The generation of the session's last attach, as core counts them
    uint32_t conditionsGenerationForTesting() const
    {
        return m_conditionsGeneration;
    }
    // The next n attaches refused where core's call is made; during, when
    // given, runs there first, as the worker's callback can meanwhile
    void failConditionsAttachesForTesting(int n,
                                          std::function<void()> during = {})
    {
        m_failAttaches = n;
        m_duringFailedAttach = std::move(during);
    }
    // The alert tones asked, and a later time for the tone's 5 s
    const AlertTone& alertToneForTesting() const { return m_tone; }
    void addToneTimeForTesting(uint64_t us) { m_toneTimeAddedForTesting += us; }
    // The samples one window can hold, from the next connect
    void setValueSampleLimitForTesting(int samples) { m_valueSampleLimit = samples; }
    static constexpr int maxInFlightForTesting() { return MaxInFlightFrames; }
    ecuio_session_t* pendingSessionForTesting() const { return m_pendingSession.load(); }
    int togetherMergesForTesting() const { return m_togetherMerges; }
    int togetherRetiredForTesting() const
    {
        return static_cast<int>(m_togetherRetired.size());
    }
    // Test seam: where recordings go instead of Documents/Havne/recordings
    void setRecordingsRootForTesting(const QString& root) { m_recordingsRoot = root; }
    // Test seam: the earlier name's recordings, instead of
    // Documents/<earlierProductName()>/recordings
    void setEarlierRecordingsRootForTesting(const QString& root)
    {
        m_earlierRecordingsRoot = root;
    }
    // Test seam: record a replay too, to compare it with its source. The
    // app never records one: it would copy the recording it plays
    void setRecordReplaysForTesting(bool on) { m_recordReplays = on; }
    // Armed no longer judges "no condition records" itself, so core's own
    // refusal at the attach reaches the user
    void skipArmedJudgmentForTesting(bool on)
    {
        m_skipArmedJudgmentForTesting = on;
    }
    // Test seam: the next load of the plugin copy with this hash fails
    void failPluginLoadForTesting(const QString& sha)
    {
        m_pluginStore.failLoadForTesting(sha);
    }
    // Test seam: the session's recorder, for core's internal.h seams
    // (a failed write); nullptr with none
    ecuio_recorder_t* recorderForTesting() const { return m_recorder; }
    // Test seam: the running session, for a recorder of the test's own
    // (an attach the session refuses); nullptr with none
    ecuio_session_t* sessionForTesting() const { return m_session; }
    // Test seam: how long Read once waits for its reply once the
    // session starts (3 s)
    void setReadOnceWaitForTesting(int ms) { m_readOnceWaitMs = ms; }
    // Test seam: a sleep of this long, added to what core's count says,
    // for the tick to find (no runner can sleep)
    void addSleptForTesting(uint64_t us) { m_sleptAddedForTesting += us; }
    // Test seam: core can't give a recorder's folder whole, so the bridge
    // reads it as empty
    void failRecorderFolderForTesting(bool on)
    {
        m_failRecorderFolderForTesting = on;
    }
};
