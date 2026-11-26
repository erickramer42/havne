// File/layer: app/exprcheck.h | Qt6 Core, app layer, C++17
// Testing: BRIDGE: BridgeTest.Expr* (test_bridge, tests/test_bridge_expr.cpp)

#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

#include <cstdint>
#include <vector>

#include "ecuio/signals.h"

// The signal form's computed-signal editor (ADR-0062's appendix of
// 2026-11-16): an expression checked, its inputs named and a value worked
// out from typed ones, all against a signal file the form hands over as
// text. Core does every judgment (ecuio_signal_file_check_expr and its
// three kin); this class marshals plain values to and from them. The four
// calls only read the file and take microseconds, so everything runs
// synchronously on the GUI thread: no worker, no session, no store.
class ExprEditor : public QObject
{
    Q_OBJECT

public:
    explicit ExprEditor(QObject* parent = nullptr);
    ~ExprEditor() override;
    ExprEditor(const ExprEditor&) = delete;
    ExprEditor& operator=(const ExprEditor&) = delete;

    // Loads text as the file the other calls read, replacing the last.
    // { ok, words }: words are core's when it refuses the file, after
    // which every call answers "no file"
    Q_INVOKABLE QVariantMap setFile(const QString& text);
    // A vehicle's merged set (SessionBridge::vehicleSetText) the other calls
    // read in place of setFile's while it is loaded; "" goes back to it
    Q_INVOKABLE QVariantMap setVehicleFile(const QString& text);
    // { ok, words, names }: the names expr reads, as written (ecuio_expr_names)
    Q_INVOKABLE QVariantMap readNames(const QString& expr) const;

    // One row for each def of the file's set, in the set's order: { ref,
    // name, unit, usable, why }. ref is the text an expression names the
    // def by; usable is false, with the words in why, for a def no
    // expression can name (obd2's Boost, a key that starts with a digit)
    // or one that reads no number (text, hex).
    Q_INVOKABLE QVariantList names() const;

    // Compiles expr as the computed signal key of the file (key "" for a
    // new one). { ok, at, words, format, depth, inputs, leaves }: at is
    // the refused character from 1, words core's; inputs and leaves are
    // [{ id, ref, name, unit }] (the program's own inputs, and the
    // numbers under it through any chain). An accepted check is the one
    // evaluate() works out.
    Q_INVOKABLE QVariantMap check(const QString& key, const QString& expr);

    // Works the last accepted check out over values, [{ ref, value,
    // ageMs }]: { ok, hasValue, value, why, leaf, leafName, boundMs,
    // words }. why is "", "missing", "stale" or "noNumber". An entry whose
    // ref is no leaf, or whose value isn't finite, is left out (core then
    // says it is missing).
    Q_INVOKABLE QVariantMap evaluate(const QVariantList& values) const;

private:
    struct Leaf {
        uint16_t id;
        QString ref;
        QString name;
    };

    QVariantMap describe(uint16_t id) const;
    QVariantMap noFile() const;
    const ecuio_signal_def_t* defOf(uint16_t id) const;
    QString refOf(const ecuio_signal_def_t& def, QString* why) const;
    void release();
    void dropProgram();
    const ecuio_signal_file_t* file() const
    {
        return m_vehicle != nullptr ? m_vehicle : m_file;
    }

    ecuio_signal_file_t* m_file = nullptr;
    ecuio_signal_file_t* m_vehicle = nullptr;
    bool m_hasProgram = false;
    ecuio_computed_t m_program{};
    std::vector<Leaf> m_leaves;
};
