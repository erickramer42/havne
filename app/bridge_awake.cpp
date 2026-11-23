// File/layer: app/bridge_awake.cpp | Qt6 Core, app layer, C++17
// Testing: BRIDGE: BridgeTest's keep-awake cases (test_bridge)

// Keep-awake (ADR-0033's appendix "the host kept awake while a session
// reads"): the connect thread takes the hold, so a take that waits on
// Linux's buses never waits on the GUI thread; the GUI thread only
// releases, which never waits. On Android the bridge never calls core:
// AndroidSession gates the service's wake lock on the switch.

#include "sessionbridge.h"

#include <utility>

void SessionBridge::setKeepAwake(bool on)
{
    if (m_keepAwake.load() == on) {
        return;
    }
    m_keepAwake.store(on);
#ifndef Q_OS_ANDROID
    if (!on && m_session != nullptr) {
        ecuio_session_keep_awake(m_session, 0, nullptr);
    }
#endif
    emit keepAwakeChanged();
}

void SessionBridge::holdAwake(ecuio_session_t* session)
{
    m_awakeRefusal.clear();
#ifndef Q_OS_ANDROID
    if (!m_keepAwake.load()) {
        return;
    }
    const ecuio_status_t rc = ecuio_session_keep_awake(
        session, ECUIO_AWAKE_SYSTEM, "Havne: reading a vehicle");
    if (rc != ECUIO_OK) {
        const char* detail = ecuio_last_error_detail();
        m_awakeRefusal =
            QStringLiteral("Couldn't keep the computer awake: %1")
                .arg(QString::fromUtf8(*detail != '\0'
                                           ? detail
                                           : ecuio_status_string(rc)));
    }
#else
    (void)session;
#endif
}

void SessionBridge::applyAwake(ecuio_session_t* session)
{
    if (!m_awakeRefusal.isEmpty()) {
        logUnnoted(std::exchange(m_awakeRefusal, QString()));
    }
#ifndef Q_OS_ANDROID
    if (!m_keepAwake.load()) {
        ecuio_session_keep_awake(session, 0, nullptr);
    }
#else
    (void)session;
#endif
}
