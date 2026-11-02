// File/layer: app/androidsession.h | Qt6 Core, app layer, C++17; built on
//             Android only
// Testing: CI's android-build compiles it; BENCH and Vehicle on the phone
//          (ADR-0029 section 5): the notification while an adapter session is up,
//          its text, none after the disconnect; while a relay runs (ADR-0047)

#pragma once

#include <QObject>
#include <QString>

class AdapterRelay;
class SessionBridge;

// Holds the foreground service (SessionService.java, ADR-0029 section 2) while an
// adapter session is up: started at the connect, its notification's text
// kept to what is connected and whether it's recording, stopped at the
// disconnect. mock:// and replay:// run without it. A relay to a computer
// (ADR-0047) holds it the same way while it runs, so hours of a sweep
// survive the screen going off. The notification permission is asked
// once, at the first adapter connect or relay.
class AndroidSession : public QObject
{
    Q_OBJECT

public:
    AndroidSession(SessionBridge* bridge, AdapterRelay* relay,
                   QObject* parent = nullptr);
    ~AndroidSession() override;

private:
    void update();
    void stop();

    SessionBridge* m_bridge;
    AdapterRelay* m_relay;
    QString m_shown; // the notification's text, "" while none is up
};
