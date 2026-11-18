// File/layer: app/recording_frameset.h | Qt6 Core, app layer, C++17
// Testing: BRIDGE: RecordingViewTest (test_bridge)
//
// What the viewer's frames open decodes with (RecordingView::openFrames):
// a registered profile's set, copied, and keyed by its signal file when
// the file's text gives that set; or a recording's own signals.json.

#pragma once

#include <QByteArray>
#include <QString>

#include <functional>
#include <memory>
#include <vector>

#include "ecuio/signals.h"

// Made on the GUI thread, touched only by the open's thread after; core
// copies the set at the open, so it need outlive only that call
struct RecordingFrameSet {
    // The text of the signal file a registered set is of, "" for a set
    // with none: a copy, so no store's file crosses to the open's thread
    using SignalFileText =
        std::function<QByteArray(const ecuio_signal_set_t* set)>;
    // Set in main() beside the bridge, which answers it. Unset, a
    // registered profile opens with its set alone ("id:<n>" for a computed
    // signal). GUI thread only
    static void setSignalFileText(SignalFileText hook);
    // The hook's text for set, "" while it's unset. GUI thread only
    static QByteArray signalFileText(const ecuio_signal_set_t* set);

    // The recording's own signals.json, loaded on the GUI thread; owned
    explicit RecordingFrameSet(ecuio_signal_file_t* own);
    // A registered set, copied: the registry's can be unregistered, and a
    // plugin's freed, while the open runs
    explicit RecordingFrameSet(const ecuio_signal_set_t& s);

    // On the open's thread: the file from fileText, kept only when its set
    // equals the copy byte for byte; else the copy opens alone
    void loadText();
    // What the open decodes with: the file's own set where there is a file
    const ecuio_signal_set_t* openSet() const;

    std::vector<ecuio_signal_def_t> defs;
    std::vector<ecuio_channel_decl_t> channels;
    std::vector<ecuio_enum_entry_t> enums;
    std::vector<ecuio_computed_t> computed;
    ecuio_signal_set_t set{};
    // Owned; when set, the open takes its set and its keys
    std::unique_ptr<ecuio_signal_file_t, void (*)(ecuio_signal_file_t*)> file{
        nullptr, ecuio_signal_file_free};
    // A registered set's signal file as text, which loadText loads
    QByteArray fileText;
    QString decodedWith;
};
