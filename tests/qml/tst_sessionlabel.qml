// File/layer: tests/qml/tst_sessionlabel.qml | Qt Quick Test, QML tier
// Testing: SessionLabel.address drops only the profile= pair from a
//          session's address and keeps the other keys. No session.

import QtQuick
import QtTest
import App
import AppTest
TestCase {
    name: "SessionLabel"

    function test_aPluginProfileIsDropped() {
        compare(SessionLabel.address("elm327://COM6?profile=plugin.example-sim.my2019"),
                "elm327://COM6")
    }

    function test_otherKeysStay() {
        compare(SessionLabel.address("elm327://COM6?speed=500000&profile=obd2&session=03"),
                "elm327://COM6?speed=500000&session=03")
        compare(SessionLabel.address("mock://?profile=obd2&did=0xF190"), "mock://?did=0xF190")
    }

    function test_anAddressWithNoProfileIsAsItWas() {
        compare(SessionLabel.address("mock://demo"), "mock://demo")
        compare(SessionLabel.address("j2534://Vendor?speed=1"), "j2534://Vendor?speed=1")
        compare(SessionLabel.address(""), "")
    }

    function test_aQueryLeftEmptyLosesItsQuestionMark() {
        compare(SessionLabel.address("mock://?profile=obd2"), "mock://")
    }

    function test_aKeyThatOnlyEndsInProfileStays() {
        compare(SessionLabel.address("mock://?subprofile=1"), "mock://?subprofile=1")
    }
}
