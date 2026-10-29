// File/layer: app/androidsession.h | Qt6 Core, app layer, C++17; built on
//             Android only
// Testing: CI's android-build compiles it; BENCH and Vehicle on the phone
//          (ADR-0029 §5): the notification while an adapter session is up,
//          its text, none after the disconnect

#pragma once

#include <QObject>
#include <QString>

class SessionBridge;

// Holds the foreground service (SessionService.java, ADR-0029 §2) while an
// adapter session is up: started at the connect, its notification's text
// kept to what is connected and whether it's recording, stopped at the
// disconnect. mock:// and replay:// run without it. The notification
// permission is asked once, at the first adapter connect.
class AndroidSession : public QObject
{
    Q_OBJECT

public:
    explicit AndroidSession(SessionBridge* bridge, QObject* parent = nullptr);
    ~AndroidSession() override;

private:
    void update();
    void stop();

    SessionBridge* m_bridge;
    QString m_shown; // the notification's text, "" while none is up
};
