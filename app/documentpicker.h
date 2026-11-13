// File/layer: app/documentpicker.h | Qt6 Core/Qml, app layer, C++17
// Testing: QML: QmlTest.documentpicker (test_qml); BENCH for Android's picker
//
// Android's own document picker (ACTION_OPEN_DOCUMENT) for the open-mode
// dialogs. Qt Quick's FileDialog checks a pick through Qt's content file
// engine, which takes a document named with parentheses ("name (1).json")
// for missing and keeps the dialog's previous pick; this hands the picked
// content:// URI on as it came, for openPicked (urlwrite.h) to read.
// Exposed to QML as the `DocumentPicker` singleton, which
// DocumentOpenDialog.qml wraps. Off Android `available` is false and the
// dialogs use Qt's FileDialog.

#pragma once

#include <QObject>
#include <QString>
#include <QUrl>
#include <QtQmlIntegration>

class DocumentPicker : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
    // This build asks Android's picker
    Q_PROPERTY(bool available READ available CONSTANT)

public:
    // Android's Activity.RESULT_OK; any other code is a cancel
    static constexpr int kResultOk = -1;

    explicit DocumentPicker(QObject* parent = nullptr);

    bool available() const;

    // Shows the picker and returns this pick's request number. At most one
    // of picked, canceled and failed answers it, later on the GUI thread; a
    // newer pick makes an older one's answer stale, and a stale answer is
    // dropped, so a pick whose answer never comes holds nothing up
    Q_INVOKABLE int pick();

    // The activity's request code for a pick: Qt keeps one callback for each
    // code, so a second pick under one code would take the first's place.
    // 256 in turn, inside the 16 bits Android allows
    Q_INVOKABLE int requestCodeOf(int request) const
    {
        return kRequestCodeBase + (request & 0xFF);
    }
    static constexpr int kRequestCodeBase = 0x4400;

    // A pick's answer as Android hands it: the result code, the document's
    // URI as text ("" for none) and its display name ("" when unknown)
    Q_INVOKABLE void deliver(int request, int resultCode, const QString& uri,
                             const QString& name);

    // Off Android, a pick waits for a deliver() as the phone's does, where it
    // would fail at once
    Q_INVOKABLE void setPickerForTesting(bool on) { m_pickerForTesting = on; }

signals:
    void picked(int request, const QUrl& url, const QString& name);
    void canceled(int request);
    void failed(int request, const QString& words);

private:
    void start(int request);
    void failLater(int request, const QString& words);

    int m_last = 0;
    int m_pending = 0;
    bool m_pickerForTesting = false;
};
