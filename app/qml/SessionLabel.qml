// File/layer: app/qml/SessionLabel.qml | Qt6 Quick, UI layer
// Testing: QML: QmlTest.sessionlabel (test_qml)

pragma Singleton
import QtQuick

// A session's address as the headers show it. Bridge.uriLabel keeps the
// whole query for logs and Settings; the headers drop the profile= pair,
// which for a plugin is a long id the vehicle's and plugin's names already
// say, and keep every other key (speed, did, session).
QtObject {
    function address(label) {
        const at = label.indexOf("?")
        if (at < 0)
            return label
        const kept = label.slice(at + 1).split("&")
                          .filter(p => p !== "" && !p.startsWith("profile="))
        return kept.length > 0 ? label.slice(0, at + 1) + kept.join("&")
                               : label.slice(0, at)
    }
}
