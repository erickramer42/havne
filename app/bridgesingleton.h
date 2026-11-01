// File/layer: app/bridgesingleton.h | Qt6 Qml, app layer, C++17
// Testing: manual smoke (the app driven through UI Automation): every
//          page binds Bridge.*; all_qmllint resolves Bridge's and Relay's
//          properties through these registrations

#pragma once

#include <QJSEngine>
#include <QQmlEngine>
#include <QtQml/qqmlregistration.h>

#include "adapterrelay.h"
#include "sessionbridge.h"

// Exposes main()'s bridge to QML as the `Bridge` singleton of the App
// module. A registered type, unlike a context property, is visible to
// qmllint and the QML compiler, and its name doesn't shadow the module's.
// The registrations live in these foreign wrappers because the bridge
// and its models stay QtCore-only (test_bridge links nothing else).
struct BridgeSingleton
{
    Q_GADGET
    QML_FOREIGN(SessionBridge)
    QML_SINGLETON
    QML_NAMED_ELEMENT(Bridge)

public:
    // main()'s bridge. Set before the engine loads any QML.
    inline static SessionBridge* instance = nullptr;

    static SessionBridge* create(QQmlEngine*, QJSEngine* engine)
    {
        Q_ASSERT(instance != nullptr);
        Q_ASSERT(engine->thread() == instance->thread());
        // main() owns it; the engine must never delete it.
        QJSEngine::setObjectOwnership(instance, QJSEngine::CppOwnership);
        return instance;
    }
};

// main()'s adapter relay as the `Relay` singleton (ADR-0047); without
// one (the QML tests'), the engine's own, which can't relay
struct RelaySingleton
{
    Q_GADGET
    QML_FOREIGN(AdapterRelay)
    QML_SINGLETON
    QML_NAMED_ELEMENT(Relay)

public:
    static AdapterRelay* create(QQmlEngine*, QJSEngine* engine)
    {
        AdapterRelay* relay = AdapterRelay::instance;
        if (relay == nullptr) {
            return new AdapterRelay; // the engine's
        }
        Q_ASSERT(engine->thread() == relay->thread());
        QJSEngine::setObjectOwnership(relay, QJSEngine::CppOwnership);
        return relay;
    }
};

// The bridge's model properties, typed for qmllint. Not creatable or
// nameable from QML.
struct FrameListModelForeign
{
    Q_GADGET
    QML_FOREIGN(FrameListModel)
    QML_ANONYMOUS
};

struct SignalListModelForeign
{
    Q_GADGET
    QML_FOREIGN(SignalListModel)
    QML_ANONYMOUS
};

struct LiveSignalModelForeign
{
    Q_GADGET
    QML_FOREIGN(LiveSignalModel)
    QML_ANONYMOUS
};
