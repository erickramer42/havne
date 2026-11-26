// File/layer: app/exprcheck.cpp | Qt6 Core, app layer, C++17
// Testing: BRIDGE: BridgeTest.Expr* (test_bridge, tests/test_bridge_expr.cpp)

#include "exprcheck.h"

#include <algorithm>
#include <cmath>

namespace {

QString detail()
{
    return QString::fromUtf8(ecuio_last_error_detail());
}

bool isNumberKind(uint8_t kind)
{
    return kind != ECUIO_VALUE_TEXT && kind != ECUIO_VALUE_HEX;
}

} // namespace

ExprEditor::ExprEditor(QObject* parent) : QObject(parent) {}

ExprEditor::~ExprEditor()
{
    release();
    ecuio_signal_file_free(m_vehicle);
}

void ExprEditor::release()
{
    ecuio_signal_file_free(m_file);
    m_file = nullptr;
    dropProgram();
}

void ExprEditor::dropProgram()
{
    m_hasProgram = false;
    m_leaves.clear();
}

QVariantMap ExprEditor::noFile() const
{
    return {{QStringLiteral("ok"), false},
            {QStringLiteral("at"), 0},
            {QStringLiteral("words"), QStringLiteral("no signal file to check against")}};
}

QVariantMap ExprEditor::setFile(const QString& text)
{
    release();
    const QByteArray bytes = text.toUtf8();
    ecuio_signal_file_t* loaded = nullptr;
    if (ecuio_signal_file_load(bytes.constData(),
                               static_cast<size_t>(bytes.size()),
                               &loaded) != ECUIO_OK) {
        return {{QStringLiteral("ok"), false}, {QStringLiteral("words"), detail()}};
    }
    m_file = loaded;
    return {{QStringLiteral("ok"), true}, {QStringLiteral("words"), QString()}};
}

QVariantMap ExprEditor::setVehicleFile(const QString& text)
{
    ecuio_signal_file_free(m_vehicle);
    m_vehicle = nullptr;
    dropProgram();
    if (text.isEmpty()) {
        return {{QStringLiteral("ok"), true},
                {QStringLiteral("words"), QString()}};
    }
    const QByteArray bytes = text.toUtf8();
    ecuio_signal_file_t* loaded = nullptr;
    if (ecuio_signal_file_load_with(
            bytes.constData(), static_cast<size_t>(bytes.size()),
            ECUIO_SIGNAL_FILE_LOAD_SET, &loaded) != ECUIO_OK) {
        return {{QStringLiteral("ok"), false},
                {QStringLiteral("words"), detail()}};
    }
    m_vehicle = loaded;
    return {{QStringLiteral("ok"), true}, {QStringLiteral("words"), QString()}};
}

QVariantMap ExprEditor::readNames(const QString& expr) const
{
    const QByteArray bytes = expr.toUtf8();
    uint32_t count = 0;
    std::vector<ecuio_expr_name_t> names;
    ecuio_status_t rc =
        ecuio_expr_names(bytes.constData(), static_cast<size_t>(bytes.size()),
                         nullptr, 0, &count);
    if (rc == ECUIO_OK && count > 0) {
        names.resize(count);
        rc = ecuio_expr_names(bytes.constData(),
                              static_cast<size_t>(bytes.size()), names.data(),
                              count, &count);
    }
    if (rc != ECUIO_OK) {
        return {{QStringLiteral("ok"), false},
                {QStringLiteral("words"), detail()},
                {QStringLiteral("names"), QStringList()}};
    }
    QStringList list;
    for (uint32_t i = 0; i < count && i < names.size(); ++i) {
        list << QString::fromUtf8(names[i].text);
    }
    return {{QStringLiteral("ok"), true},
            {QStringLiteral("words"), QString()},
            {QStringLiteral("names"), list}};
}

const ecuio_signal_def_t* ExprEditor::defOf(uint16_t id) const
{
    const ecuio_signal_set_t* set = ecuio_signal_file_set(file());
    for (uint32_t i = 0; set != nullptr && i < set->count; ++i) {
        if (set->defs[i].id == id) {
            return &set->defs[i];
        }
    }
    return nullptr;
}

QString ExprEditor::refOf(const ecuio_signal_def_t& def, QString* why) const
{
    char out[ECUIO_EXPR_REF_LEN] = {};
    if (ecuio_signal_file_expr_ref(file(), &def, out, sizeof out) != ECUIO_OK) {
        if (why != nullptr) {
            *why = detail();
        }
        return QString();
    }
    return QString::fromUtf8(out);
}

QVariantList ExprEditor::names() const
{
    QVariantList rows;
    const ecuio_signal_set_t* set = ecuio_signal_file_set(file());
    for (uint32_t i = 0; set != nullptr && i < set->count; ++i) {
        const ecuio_signal_def_t& def = set->defs[i];
        QString why;
        const QString ref = refOf(def, &why);
        bool usable = !ref.isEmpty();
        if (usable && !isNumberKind(def.kind)) {
            usable = false;
            why = QStringLiteral("text: no number to compute with");
        }
        rows.append(QVariantMap{
            {QStringLiteral("ref"), ref},
            {QStringLiteral("name"), QString::fromUtf8(def.name)},
            {QStringLiteral("unit"), QString::fromUtf8(def.unit)},
            {QStringLiteral("usable"), usable},
            {QStringLiteral("why"), usable ? QString() : why}});
    }
    return rows;
}

QVariantMap ExprEditor::describe(uint16_t id) const
{
    QVariantMap row{{QStringLiteral("id"), id}};
    const ecuio_signal_def_t* def = defOf(id);
    if (def != nullptr) {
        row.insert(QStringLiteral("ref"), refOf(*def, nullptr));
        row.insert(QStringLiteral("name"), QString::fromUtf8(def->name));
        row.insert(QStringLiteral("unit"), QString::fromUtf8(def->unit));
    }
    return row;
}

QVariantMap ExprEditor::check(const QString& key, const QString& expr)
{
    dropProgram();
    if (file() == nullptr) {
        return noFile();
    }
    const QByteArray keyBytes = key.toUtf8();
    const QByteArray exprBytes = expr.toUtf8();
    ecuio_expr_check_t out{};
    out.struct_size = sizeof out;
    const ecuio_status_t rc = ecuio_signal_file_check_expr(
        file(), keyBytes.isEmpty() ? nullptr : keyBytes.constData(),
        exprBytes.constData(), static_cast<size_t>(exprBytes.size()), &out);
    if (rc != ECUIO_OK) {
        return {{QStringLiteral("ok"), false},
                {QStringLiteral("at"), static_cast<int>(out.at)},
                {QStringLiteral("words"), detail()}};
    }
    m_program = out.program;
    m_hasProgram = true;

    QVariantList inputs;
    for (uint8_t i = 0; i < out.program.input_count; ++i) {
        inputs.append(describe(out.program.input_ids[i]));
    }
    uint32_t count = 0;
    std::vector<uint16_t> ids;
    if (ecuio_signal_file_expr_leaves(file(), &out.program, nullptr, 0,
                                      &count) == ECUIO_OK &&
        count > 0) {
        ids.resize(count);
        uint32_t filled = 0;
        if (ecuio_signal_file_expr_leaves(file(), &out.program, ids.data(),
                                          count, &filled) != ECUIO_OK) {
            ids.clear();
        }
    }
    QVariantList leaves;
    for (const uint16_t id : ids) {
        const QVariantMap row = describe(id);
        leaves.append(row);
        m_leaves.push_back({id, row.value(QStringLiteral("ref")).toString(),
                            row.value(QStringLiteral("name")).toString()});
    }
    return {{QStringLiteral("ok"), true},
            {QStringLiteral("at"), 0},
            {QStringLiteral("words"), QString()},
            {QStringLiteral("format"), static_cast<int>(out.format)},
            {QStringLiteral("depth"), static_cast<int>(out.depth)},
            {QStringLiteral("inputs"), inputs},
            {QStringLiteral("leaves"), leaves}};
}

QVariantMap ExprEditor::evaluate(const QVariantList& values) const
{
    if (file() == nullptr || !m_hasProgram) {
        return noFile();
    }
    std::vector<ecuio_eval_input_t> inputs;
    for (const QVariant& v : values) {
        const QVariantMap m = v.toMap();
        const QString ref = m.value(QStringLiteral("ref")).toString();
        bool ok = false;
        const double value = m.value(QStringLiteral("value")).toDouble(&ok);
        if (!ok || !std::isfinite(value)) {
            continue;
        }
        const double age = m.value(QStringLiteral("ageMs")).toDouble();
        for (const Leaf& leaf : m_leaves) {
            if (leaf.ref == ref) {
                ecuio_eval_input_t in{};
                in.value = value;
                // 1e12 ms keeps the cast to microseconds inside uint64_t
                in.age_us = age > 0 && std::isfinite(age)
                                ? static_cast<uint64_t>(std::min(age, 1e12) * 1000.0) : 0;
                in.def_id = leaf.id;
                inputs.push_back(in);
                break;
            }
        }
    }
    ecuio_eval_result_t out{};
    out.struct_size = sizeof out;
    if (ecuio_signal_file_evaluate(file(), &m_program, inputs.data(),
                                   static_cast<uint32_t>(inputs.size()),
                                   &out) != ECUIO_OK) {
        return {{QStringLiteral("ok"), false}, {QStringLiteral("words"), detail()}};
    }
    QString why;
    switch (out.why) {
    case ECUIO_EVAL_WHY_MISSING: why = QStringLiteral("missing"); break;
    case ECUIO_EVAL_WHY_STALE: why = QStringLiteral("stale"); break;
    case ECUIO_EVAL_WHY_NO_NUMBER: why = QStringLiteral("noNumber"); break;
    default: break;
    }
    QString leafName;
    for (const Leaf& leaf : m_leaves) {
        if (out.leaf != 0 && leaf.id == out.leaf) {
            leafName = leaf.name;
        }
    }
    const bool has = (out.flags & ECUIO_EVAL_VALUE) != 0;
    return {{QStringLiteral("ok"), true},
            {QStringLiteral("hasValue"), has},
            {QStringLiteral("value"), has ? out.value : 0.0},
            {QStringLiteral("why"), why},
            {QStringLiteral("leaf"), static_cast<int>(out.leaf)},
            {QStringLiteral("leafName"), leafName},
            {QStringLiteral("boundMs"), static_cast<double>(out.bound_us) / 1000.0},
            {QStringLiteral("words"), QString()}};
}
