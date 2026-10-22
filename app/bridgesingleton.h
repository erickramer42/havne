// File/layer: app/bridgesingleton.h | Qt6 Qml, app layer, C++17
// Testing: manual smoke (run-qttune skill): every page binds Bridge.*;
//          all_qmllint resolves Bridge's properties through this
//          registration

#pragma once

#include <QJSEngine>
#include <QQmlEngine>
#include <QtQml/qqmlregistration.h>

#include "qttunebridge.h"

// Exposes main()'s bridge to QML as the `Bridge` singleton of the QtTune
// module. A registered type, unlike a context property, is visible to
// qmllint and the QML compiler, and its name doesn't shadow the module's.
// The registrations live in these foreign wrappers because the bridge
// and its models stay QtCore-only (test_bridge links nothing else).
struct BridgeSingleton
{
    Q_GADGET
    QML_FOREIGN(QtTuneBridge)
    QML_SINGLETON
    QML_NAMED_ELEMENT(Bridge)

public:
    // main()'s bridge. Set before the engine loads any QML.
    inline static QtTuneBridge* instance = nullptr;

    static QtTuneBridge* create(QQmlEngine*, QJSEngine* engine)
    {
        Q_ASSERT(instance != nullptr);
        Q_ASSERT(engine->thread() == instance->thread());
        // main() owns it; the engine must never delete it.
        QJSEngine::setObjectOwnership(instance, QJSEngine::CppOwnership);
        return instance;
    }
};

// The bridge's model properties, typed for qmllint. Not creatable or
// nameable from QML.
struct FrameSortProxyForeign
{
    Q_GADGET
    QML_FOREIGN(FrameSortProxy)
    QML_ANONYMOUS
};

struct SignalListModelForeign
{
    Q_GADGET
    QML_FOREIGN(SignalListModel)
    QML_ANONYMOUS
};
