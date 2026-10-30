// File/layer: app/btconnect.cpp | C++17, app layer, no Qt
// Testing: UNIT: BridgeTest.BtConnect* (test_bridge)

#include "btconnect.h"

using Action = BtConnectRules::Action;

Action BtConnectRules::open()
{
    m_wanted = true;
    if (m_up) {
        // Core closes before it opens again; an open on an open link
        // starts over
        m_up = false;
        m_connecting = true;
        m_adopted = false;
        return Action::Connect;
    }
    if (m_connecting) {
        m_adopted = true; // the running connect is this open's now
        return Action::None;
    }
    m_connecting = true;
    m_adopted = false;
    return Action::Connect;
}

Action BtConnectRules::close()
{
    m_wanted = false;
    if (m_up) {
        m_up = false;
        return Action::CloseSocket;
    }
    // A running connect is left to finish: an abort can't cut it short
    return Action::None;
}

Action BtConnectRules::connected()
{
    if (!m_connecting) {
        return Action::None;
    }
    m_connecting = false;
    m_adopted = false;
    if (!m_wanted) {
        return Action::CloseSocket;
    }
    m_wanted = false;
    m_up = true;
    return Action::ReportOpened;
}

Action BtConnectRules::connectFailed()
{
    if (!m_connecting) {
        return Action::None;
    }
    m_connecting = false;
    if (!m_wanted) {
        m_adopted = false;
        return Action::None;
    }
    if (m_adopted) {
        // It began for an open core has given up on; the one waiting
        // gets a connect of its own
        m_adopted = false;
        m_connecting = true;
        return Action::Connect;
    }
    m_wanted = false;
    return Action::ReportFailed;
}

Action BtConnectRules::lost()
{
    if (!m_up) {
        return Action::None;
    }
    m_up = false;
    return Action::ReportFailed;
}
