// File/layer: app/androidsession.cpp | Qt6 Core, app layer, C++17; built on
//             Android only
// Testing: CI's android-build compiles it; BENCH and Vehicle on the phone
//          (ADR-0029 §5)

#include "androidsession.h"
#include "sessionbridge.h"

#include <QJniObject>
#include <QtCore/qcoreapplication_platform.h>

namespace {

constexpr const char* kService = "io/github/erickramer42/havne/SessionService";

QJniObject appContext()
{
    return QNativeInterface::QAndroidApplication::context();
}

} // namespace

AndroidSession::AndroidSession(SessionBridge* bridge, QObject* parent)
    : QObject(parent), m_bridge(bridge)
{
    connect(bridge, &SessionBridge::connectedChanged, this,
            &AndroidSession::update);
    connect(bridge, &SessionBridge::currentUriChanged, this,
            &AndroidSession::update);
    connect(bridge, &SessionBridge::recordingStateChanged, this,
            &AndroidSession::update);
}

AndroidSession::~AndroidSession()
{
    stop();
}

void AndroidSession::update()
{
    const bool adapter = m_bridge->connected() &&
                         m_bridge->currentUri().startsWith(
                             QLatin1String("elm327://"), Qt::CaseInsensitive);
    if (!adapter) {
        stop();
        return;
    }
    QString text = tr("Connected to %1").arg(m_bridge->uriLabel());
    if (m_bridge->recordingActive()) {
        text = tr("%1 · recording").arg(text);
    }
    if (text == m_shown) {
        return;
    }
    const QJniObject context = appContext();
    if (m_shown.isEmpty()) {
        QJniObject::callStaticMethod<void>(kService, "askToNotifyOnce",
                                           "(Landroid/content/Context;)V",
                                           context.object());
    }
    QJniObject::callStaticMethod<void>(
        kService, "start", "(Landroid/content/Context;Ljava/lang/String;)V",
        context.object(), QJniObject::fromString(text).object<jstring>());
    m_shown = text;
}

void AndroidSession::stop()
{
    if (m_shown.isEmpty()) {
        return;
    }
    QJniObject::callStaticMethod<void>(kService, "stop",
                                       "(Landroid/content/Context;)V",
                                       appContext().object());
    m_shown.clear();
}
