// File/layer: tests/test_bridge_variantshape.cpp | GTest + QtCore, app-layer tests
// Testing: BRIDGE: BridgeTest.VariantShape* (test_bridge)
//
// What a file on obd2 adds and what it replaces (app/variantshape.h), as
// both stores say it for the Signals list's rows.

#include <gtest/gtest.h>

#include <QStringList>
#include <QTemporaryDir>
#include <QVariantList>

#include <cstring>
#include <functional>
#include <vector>

#include "sessionbridge.h"
#include "variantshape.h"
#include <ecuio/profile.h>
#include <ecuio/signals.h>
#include "bridge_support.h"

namespace {

// A file on obd2 whose signals are the given names, each reading a DID of
// the engine ECU
QByteArray fileOf(const QString& name, const QStringList& signalNames)
{
    QStringList signalsText;
    int n = 0;
    for (const QString& s : signalNames) {
        signalsText << QStringLiteral(
                           R"({"key":"k%1","name":"%2","read":{"service":"22",)"
                           R"("number":"D1%3"},"field":{"byte":0,"bits":8}})")
                           .arg(n)
                           .arg(s)
                           .arg(n, 2, 16, QLatin1Char('0'));
        ++n;
    }
    return QStringLiteral(
               R"({"havne_signals":1,"name":"%1","source":{"name":"t"},"extends":"obd2",)"
               R"("ecus":[{"id":"pcm","rx":"7E8"}],"signals":[%2]})")
        .arg(name, signalsText.join(QLatin1Char(',')))
        .toUtf8();
}

struct Seen
{
    int count = -1;
    int own = -1;
    QStringList replaced;
};

Seen viaSignalStore(const QByteArray& bytes)
{
    SettingsDirScope dir;
    QTemporaryDir from;
    SessionBridge bridge;
    const QVariantMap r = bridge.importSignalFile(
        writeScratch(from.path(), QStringLiteral("f.json"), bytes));
    EXPECT_TRUE(r.value(QStringLiteral("ok")).toBool())
        << r.value(QStringLiteral("words")).toString().toStdString();
    return {r.value(QStringLiteral("count")).toInt(),
            r.value(QStringLiteral("own")).toInt(),
            r.value(QStringLiteral("replacedNames")).toStringList()};
}

Seen viaPluginStore(const QByteArray& bytes)
{
    SettingsDirScope dir;
    QTemporaryDir from;
    SessionBridge bridge;
    QVariantMap r = bridge.readImport(
        writeScratch(from.path(), QStringLiteral("f.json"), bytes));
    EXPECT_TRUE(r.value(QStringLiteral("ok")).toBool())
        << r.value(QStringLiteral("words")).toString().toStdString();
    r = bridge.keepSignals(QString(), QString(), QStringLiteral("v1"));
    EXPECT_TRUE(r.value(QStringLiteral("ok")).toBool())
        << r.value(QStringLiteral("words")).toString().toStdString();
    return {r.value(QStringLiteral("counts")).toList().value(0).toInt(),
            r.value(QStringLiteral("owns")).toList().value(0).toInt(),
            r.value(QStringLiteral("replacedNames")).toList().value(0).toStringList()};
}

int obd2Count()
{
    const ecuio_signal_set_t* obd2 = nullptr;
    EXPECT_EQ(ecuio_profile_lookup("obd2", &obd2), ECUIO_OK);
    return static_cast<int>(obd2->count);
}

} // namespace

/* A signal named as one of obd2's takes its place: the set's count then
 * no longer says what the file added, so each store names the replaced and
 * counts the rest */
TEST(BridgeTest, VariantShapeAFileThatOnlyAddsOrOnlyReplacesOrBoth)
{
    const int base = obd2Count();
    for (const auto& store : {viaSignalStore, viaPluginStore}) {
        Seen s = store(fileOf(QStringLiteral("Adds"), {QStringLiteral("Mine")}));
        EXPECT_EQ(s.count, base + 1);
        EXPECT_EQ(s.own, 1);
        EXPECT_TRUE(s.replaced.isEmpty());

        s = store(fileOf(QStringLiteral("Swaps"), {QStringLiteral("RPM")}));
        EXPECT_EQ(s.count, base) << "one out, one in";
        EXPECT_EQ(s.own, 0) << "nothing beyond what it replaced";
        EXPECT_EQ(s.replaced, QStringList{QStringLiteral("RPM")});

        s = store(fileOf(QStringLiteral("Both"),
                         {QStringLiteral("Mine"), QStringLiteral("RPM"),
                          QStringLiteral("Vehicle Speed")}));
        EXPECT_EQ(s.count, base + 1);
        EXPECT_EQ(s.own, 1);
        EXPECT_EQ(s.replaced,
                  (QStringList{QStringLiteral("RPM"), QStringLiteral("Vehicle Speed")}))
            << "in the file's order";
    }
}

/* A file that extends nothing has no base to replace: all its signals are
 * its own */
TEST(BridgeTest, VariantShapeAFileOnNoBaseAddsEverySignal)
{
    QByteArray alone = fileOf(QStringLiteral("Alone"), {QStringLiteral("RPM")});
    alone.replace(R"("extends":"obd2",)", "");
    for (const auto& store : {viaSignalStore, viaPluginStore}) {
        const Seen s = store(alone);
        EXPECT_EQ(s.count, 1);
        EXPECT_EQ(s.own, 1);
        EXPECT_TRUE(s.replaced.isEmpty());
    }
}

/* sameDef reads everything the decoder and a display use: a copy of a
 * base def is the same, and a change to any one field is not */
TEST(BridgeTest, VariantShapeADefIsTheSameUntilAFieldItShowsOrDecodesDiffers)
{
    const ecuio_signal_set_t* obd2 = nullptr;
    ASSERT_EQ(ecuio_profile_lookup("obd2", &obd2), ECUIO_OK);
    const ecuio_signal_def_t& base = obd2->defs[0];
    ecuio_signal_def_t copy = base;
    EXPECT_TRUE(variant_shape::sameDef(*obd2, base, *obd2, copy));

    const std::function<void(ecuio_signal_def_t&)> changes[] = {
        [](ecuio_signal_def_t& d) { d.message_id += 1; },
        [](ecuio_signal_def_t& d) { d.mux_offset += 1; },
        [](ecuio_signal_def_t& d) { d.mux_length = 1; },
        [](ecuio_signal_def_t& d) { d.mux_key += 1; },
        [](ecuio_signal_def_t& d) { d.kind = ECUIO_VALUE_INT; },
        [](ecuio_signal_def_t& d) { d.byte_order ^= 1; },
        [](ecuio_signal_def_t& d) { d.start_byte += 1; },
        [](ecuio_signal_def_t& d) { d.bit_offset += 1; },
        [](ecuio_signal_def_t& d) { d.bit_length += 1; },
        [](ecuio_signal_def_t& d) { d.byte_length += 1; },
        [](ecuio_signal_def_t& d) { d.scale *= 2; },
        [](ecuio_signal_def_t& d) { d.offset += 1; },
        [](ecuio_signal_def_t& d) { d.name[0] = 'Z'; },
        [](ecuio_signal_def_t& d) { d.unit[0] = 'Z'; },
        [](ecuio_signal_def_t& d) { d.decimal_places += 1; },
        [](ecuio_signal_def_t& d) { d.plot_rank += 1; },
        [](ecuio_signal_def_t& d) { d.group += 1; },
        [](ecuio_signal_def_t& d) { d.flags ^= 0x80000000u; },
    };
    int i = 0;
    for (const auto& change : changes) {
        copy = base;
        change(copy);
        EXPECT_FALSE(variant_shape::sameDef(*obd2, base, *obd2, copy))
            << "change " << i;
        ++i;
    }
}

/* A computed def is the same only with the same program */
TEST(BridgeTest, VariantShapeAComputedDefIsComparedByItsProgram)
{
    const ecuio_signal_set_t* obd2 = nullptr;
    ASSERT_EQ(ecuio_profile_lookup("obd2", &obd2), ECUIO_OK);
    const ecuio_signal_def_t* boost = nullptr;
    for (uint32_t i = 0; i < obd2->count; ++i) {
        if ((obd2->defs[i].flags & ECUIO_DEF_FLAG_COMPUTED) != 0) {
            boost = &obd2->defs[i];
        }
    }
    ASSERT_NE(boost, nullptr);
    std::vector<ecuio_computed_t> other(obd2->computed,
                                        obd2->computed + obd2->computed_count);
    ecuio_signal_set_t set = *obd2;
    set.computed = other.data();
    ecuio_computed_t& program = other[boost->table_first];
    EXPECT_TRUE(variant_shape::sameDef(*obd2, *boost, set, *boost));

    // Each field of the program, each element of each array, alone
    std::vector<std::function<void()>> changes;
    changes.push_back([&] { program.input_count += 1; });
    changes.push_back([&] { program.step_count += 1; });
    changes.push_back([&] { program.constant_count += 1; });
    for (size_t i = 0; i < 4; ++i) {
        changes.push_back([&, i] { program.input_ids[i] += 1; });
        changes.push_back([&, i] { program.max_age_us[i] += 1; });
    }
    for (size_t i = 0; i < sizeof program.steps; ++i) {
        changes.push_back([&, i] { program.steps[i] ^= 1; });
    }
    for (size_t i = 0; i < 16; ++i) {
        changes.push_back([&, i] { program.constants[i] += 1.0; });
    }
    int i = 0;
    for (const auto& change : changes) {
        const ecuio_computed_t was = program;
        change();
        EXPECT_FALSE(variant_shape::sameDef(*obd2, *boost, set, *boost))
            << "change " << i;
        program = was;
        ++i;
    }
    // Constants are compared as numbers, not as bytes: -0 is +0
    std::vector<ecuio_computed_t> plus = other;
    std::vector<ecuio_computed_t> minus = other;
    plus[boost->table_first].constants[0] = 0.0;
    minus[boost->table_first].constants[0] = -0.0;
    ecuio_signal_set_t plusSet = *obd2;
    ecuio_signal_set_t minusSet = *obd2;
    plusSet.computed = plus.data();
    minusSet.computed = minus.data();
    EXPECT_TRUE(variant_shape::sameDef(plusSet, *boost, minusSet, *boost));
}

/* An enum def is the same only with the same entries */
TEST(BridgeTest, VariantShapeAnEnumDefIsComparedByItsEntries)
{
    ecuio_enum_entry_t one[2] = {};
    one[0].raw = 0;
    std::strcpy(one[0].label, "Off");
    one[1].raw = 1;
    std::strcpy(one[1].label, "On");
    ecuio_enum_entry_t two[2] = {one[0], one[1]};
    ecuio_signal_def_t def = {};
    def.kind = ECUIO_VALUE_ENUM;
    def.bit_length = 8;
    def.scale = 1.0;
    def.table_first = 0;
    def.table_count = 2;
    ecuio_signal_set_t a = {};
    a.enum_count = 2;
    a.enums = one;
    ecuio_signal_set_t b = a;
    b.enums = two;
    EXPECT_TRUE(variant_shape::sameDef(a, def, b, def));
    std::strcpy(two[1].label, "Auto");
    EXPECT_FALSE(variant_shape::sameDef(a, def, b, def)) << "a label";
    two[1] = one[1];
    two[1].raw = 2;
    EXPECT_FALSE(variant_shape::sameDef(a, def, b, def)) << "a raw value";
    two[1] = one[1];
    ecuio_signal_def_t fewer = def;
    fewer.table_count = 1;
    EXPECT_FALSE(variant_shape::sameDef(a, def, b, fewer)) << "an entry fewer";
}

/* A replacement that is the base's def over again is neither added nor
 * replaced: a file judged against its own set finds each of its own
 * signals in the base, unchanged */
TEST(BridgeTest, VariantShapeAnIdenticalReplacementIsNeitherAddedNorReplaced)
{
    SettingsDirScope dir;
    const QByteArray bytes = fileOf(QStringLiteral("Same"), {QStringLiteral("RPM")});
    ecuio_signal_file_t* file = nullptr;
    ASSERT_EQ(ecuio_signal_file_load(bytes.constData(),
                                     static_cast<size_t>(bytes.size()), &file),
              ECUIO_OK);
    const ecuio_signal_set_t* set = ecuio_signal_file_set(file);
    const variant_shape::Shape s = variant_shape::of(file, set);
    EXPECT_EQ(s.count, static_cast<int>(set->count));
    EXPECT_EQ(s.added, 0);
    EXPECT_TRUE(s.replaced.isEmpty());
    EXPECT_EQ(variant_shape::of(file, nullptr).added, 1) << "no base: its own";
    ecuio_signal_file_free(file);
}

/* A def that gives its signal a gauge differs from the base's, which has none:
 * the same file with a gauge, judged against its own set, replaces */
TEST(BridgeTest, VariantShapeAGaugeAloneMakesAReplacement)
{
    SettingsDirScope dir;
    QByteArray bytes = fileOf(QStringLiteral("Gauged"), {QStringLiteral("RPM")});
    bytes.replace(R"("field":{"byte":0,"bits":8}})",
                  R"("field":{"byte":0,"bits":8},"gauge":{"min":0,"max":100}})");
    ecuio_signal_file_t* file = nullptr;
    ASSERT_EQ(ecuio_signal_file_load(bytes.constData(),
                                     static_cast<size_t>(bytes.size()), &file),
              ECUIO_OK);
    const variant_shape::Shape s =
        variant_shape::of(file, ecuio_signal_file_set(file));
    EXPECT_EQ(s.added, 0);
    EXPECT_EQ(s.replaced, QStringList{QStringLiteral("RPM")});
    ecuio_signal_file_free(file);
}
