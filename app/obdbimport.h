// File/layer: app/obdbimport.h | Qt6 Core, app layer, C++17
// Testing: BRIDGE: ObdbImportTest, BridgeTest.Obdb* (test_bridge)
//
// An OBDb signal set imported in the app (ADR-0051 section 7 and its
// appendix of 2026-11-06): core's converter (ecuio_obdb_import) runs on
// the picked file's bytes and its notes come back sorted for Settings'
// report, each field that differs and each signal left out, before the
// converted file is kept as any signal file is (SignalStore). The words
// are core's, and never name the file's path.

#pragma once

#include <QByteArray>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

#include <cstdint>

class ObdbImport
{
public:
    // A set read: the converted file's text, and the notes by kind
    // (ecuio_obdb_note_t), each a map. converted counts the signals in
    // the file; changed has { line, signal, key, field, text } for each
    // field that differs, skipped { line, signal, text } for each signal
    // left out, file { line, field, text } for the set's own keys
    struct Preview {
        bool ok = false;
        QString words; // why not, when !ok
        QByteArray json;
        int converted = 0;
        QVariantList changed;
        QVariantList skipped;
        QVariantList file;
    };

    // The set's bytes converted for modelYear (0: none, which leaves out
    // every command OBDb gives for some years only), the file named name
    // (core's "OBDb import" when empty). The file extends obd2, so the
    // vehicle keeps Service 01's signals beside the set's
    static Preview read(const QByteArray& bytes, uint32_t modelYear,
                        const QString& name);

    // The preview as Settings shows it: { ok, words, name, year,
    // converted, changed, skipped, file }
    static QVariantMap report(const Preview& p, const QString& name,
                              uint32_t modelYear);

    // A signal file's bytes the converter wrote: its source block is the
    // one ecuio_obdb_import gives every file, so a re-import may replace
    // it and a file the user named and wrote is never taken for one
    static bool isConversion(const QByteArray& bytes);

    // The model year a VIN gives by ADR-0043's decode (spaces dropped,
    // upper-cased), latestModelYear the newest it may be; 0 when it isn't
    // a VIN or position 10 isn't a year code
    static int modelYear(const QString& vin, int latestModelYear);
};
