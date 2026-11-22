// File/layer: tests/test_bridge_ticks.cpp | GTest + QtCore, app-layer tests
// Testing: BRIDGE: BridgeTest: a vehicle's ticks by key (test_bridge)
//
// Cases for the signal model's keys and the lists a vehicle keeps: each
// row's key, lists read by key or by name, the texts no row answers and
// the find rule against core's.

#include <gtest/gtest.h>

#include <QByteArray>
#include <QStringList>

#include <cstring>
#include <iterator>
#include <string>
#include <vector>

#include "sessionbridge.h"
#include <ecuio/profile.h>
#include <ecuio/signal_sets.h>
#include <ecuio/signals.h>
#include "bridge_support.h"
#include "choicerows.h"

/* A row's key is core's for its def (ADR-0065 section 9), and a vehicle's
 * lists are those keys: lists kept by name tick the same rows and read
 * back as keys, which is how a book written before keys moves to them */
TEST(BridgeTest, AVehiclesTicksAreKeptByKey)
{
    SignalListModel model;
    const ecuio_signal_set_t* set = ecuio_obd2_signal_set();
    model.setSignalSet(set);
    std::vector<ecuio_signal_key_t> keys(set->count);
    uint32_t count = 0;
    ASSERT_EQ(
        ecuio_signal_set_keys(set, nullptr, keys.data(), set->count, &count),
        ECUIO_OK);
    ASSERT_EQ(static_cast<int>(count), model.rowCount());
    const auto key = [&model](int row) {
        return model.data(model.index(row), SignalListModel::KeyRole)
            .toString();
    };
    for (int r = 0; r < model.rowCount(); ++r) {
        EXPECT_EQ(key(r), QString::fromUtf8(keys[static_cast<size_t>(r)].text))
            << r;
    }
    const int rpm = obd2RowOf(ECUIO_OBD2_SIG_RPM_ID);
    const int load = obd2RowOf(ECUIO_OBD2_SIG_ENGINE_LOAD_ID);
    ASSERT_GE(rpm, 0);
    ASSERT_GE(load, 0);
    EXPECT_EQ(key(rpm), QStringLiteral("7E8:410C:2.0:16"));
    const auto plotted = [&model](int row) {
        return model.data(model.index(row), SignalListModel::SubscribedRole)
            .toBool();
    };

    model.setPlotChoices({QStringLiteral("Engine Load")},
                         {QStringLiteral("RPM")});
    EXPECT_TRUE(plotted(load));
    EXPECT_FALSE(plotted(rpm));
    EXPECT_EQ(model.plottedKeys(), QStringList{key(load)})
        << "a list of names reads back as keys";
    EXPECT_EQ(model.unplottedKeys(), QStringList{key(rpm)});

    model.setPlotChoices({}, {});
    EXPECT_FALSE(plotted(load));
    EXPECT_TRUE(plotted(rpm));
    model.setPlotChoices({key(load)}, {key(rpm)});
    EXPECT_TRUE(plotted(load)) << "by key, the same rows";
    EXPECT_FALSE(plotted(rpm));

    model.setPlotChoices({key(load), QStringLiteral("Engine Load")}, {});
    EXPECT_EQ(model.plottedKeys(), QStringList{key(load)})
        << "a row named twice is kept once";
}

/* A def the file keys itself (a computed signal) is kept under the
 * file's key, as a recording of the file writes it; the set alone gives
 * it "id:<n>" */
TEST(BridgeTest, ARowsKeyIsItsFilesOwn)
{
    const char text[] =
        R"({"havne_signals": 1, "name": "Keys", "extends": "obd2",
            "source": {"name": "Written by hand for Havne's tests"},
            "signals": [{"key": "krpm", "name": "RPM in Thousands",
                         "kind": "computed", "expr": "obd2.rpm / 1000",
                         "max_age": {"obd2.rpm": 2}, "unit": "krpm",
                         "decimals": 2, "group": "engine"}]})";
    ecuio_signal_file_t* file = nullptr;
    ASSERT_EQ(ecuio_signal_file_load(text, sizeof text - 1, &file), ECUIO_OK)
        << ecuio_last_error_detail();
    const ecuio_signal_set_t* set = ecuio_signal_file_set(file);
    const auto keyOf = [](const SignalListModel& model, const char* name) {
        for (int r = 0; r < model.rowCount(); ++r) {
            const QModelIndex i = model.index(r);
            if (model.data(i, SignalListModel::NameRole).toString() ==
                QLatin1String(name)) {
                return model.data(i, SignalListModel::KeyRole).toString();
            }
        }
        return QStringLiteral("no such row");
    };
    {
        SignalListModel model;
        model.setSignalSet(set, file);
        EXPECT_EQ(keyOf(model, "RPM in Thousands"), QStringLiteral("krpm"));
        EXPECT_EQ(keyOf(model, "RPM"), QStringLiteral("7E8:410C:2.0:16"));
        model.setPlotChoices({QStringLiteral("krpm")}, {});
        EXPECT_EQ(model.plottedKeys(), QStringList{QStringLiteral("krpm")});
        EXPECT_EQ(model.tickedIds().size(), 1u);
    }
    {
        SignalListModel model;
        model.setSignalSet(set);
        EXPECT_TRUE(
            keyOf(model, "RPM in Thousands").startsWith(QStringLiteral("id:")))
            << keyOf(model, "RPM in Thousands").toStdString();
    }
    ecuio_signal_file_free(file);
}

/* Where core refuses the keys (out of memory; here a file of another set)
 * no row has one: a text still ticks its row by name and is carried, so
 * a save keeps it */
TEST(BridgeTest, ATickOnARowWithNoKeyIsCarried)
{
    const char text[] =
        R"({"havne_signals": 1, "name": "Other", "extends": "obd2",
            "source": {"name": "Written by hand for Havne's tests"},
            "signals": [{"key": "krpm", "name": "RPM in Thousands",
                         "kind": "computed", "expr": "obd2.rpm / 1000",
                         "max_age": {"obd2.rpm": 2}, "unit": "krpm",
                         "decimals": 2, "group": "engine"}]})";
    ecuio_signal_file_t* other = nullptr;
    ASSERT_EQ(ecuio_signal_file_load(text, sizeof text - 1, &other), ECUIO_OK)
        << ecuio_last_error_detail();
    SignalListModel model;
    model.setSignalSet(ecuio_obd2_signal_set(), other);
    ecuio_signal_file_free(other);
    const int rpm = obd2RowOf(ECUIO_OBD2_SIG_RPM_ID);
    const int load = obd2RowOf(ECUIO_OBD2_SIG_ENGINE_LOAD_ID);
    ASSERT_GE(rpm, 0);
    ASSERT_GE(load, 0);
    for (int r = 0; r < model.rowCount(); ++r) {
        ASSERT_TRUE(model.data(model.index(r), SignalListModel::KeyRole)
                        .toString()
                        .isEmpty())
            << r;
    }
    const auto plotted = [&model](int row) {
        return model.data(model.index(row), SignalListModel::SubscribedRole)
            .toBool();
    };

    model.setPlotChoices({QStringLiteral("Engine Load")},
                         {QStringLiteral("RPM")});
    EXPECT_TRUE(plotted(load));
    EXPECT_FALSE(plotted(rpm));
    EXPECT_EQ(model.plottedKeys(), QStringList{QStringLiteral("Engine Load")});
    EXPECT_EQ(model.unplottedKeys(), QStringList{QStringLiteral("RPM")});
}

/* A kept tick no row answers (its signal is in another plugin's set) is
 * carried: the lists give it back after the rows' keys, so a save under
 * this set doesn't lose it and a later set that has the signal ticks it */
TEST(BridgeTest, ATickNoRowAnswersIsCarried)
{
    SignalListModel model;
    model.setSignalSet(ecuio_obd2_signal_set());
    const int rpm = obd2RowOf(ECUIO_OBD2_SIG_RPM_ID);
    const int load = obd2RowOf(ECUIO_OBD2_SIG_ENGINE_LOAD_ID);
    const auto key = [&model](int row) {
        return model.data(model.index(row), SignalListModel::KeyRole)
            .toString();
    };
    const auto plotted = [&model](int row) {
        return model.data(model.index(row), SignalListModel::SubscribedRole)
            .toBool();
    };
    const QString loadKey = key(load);
    const QString rpmKey = key(rpm);
    const QString other = QStringLiteral("7E9:62F410:3.0:16");
    const QString gone = QStringLiteral("gone");

    model.setPlotChoices({other, loadKey, other}, {gone, rpmKey});
    EXPECT_EQ(model.plottedKeys(), (QStringList{loadKey, other}))
        << "after the rows' keys, each once";
    EXPECT_EQ(model.unplottedKeys(), (QStringList{rpmKey, gone}));

    model.toggleSubscribed(rpm);
    EXPECT_EQ(model.unplottedKeys(), QStringList{gone})
        << "a tick changes the rows' part alone";
    EXPECT_EQ(model.plottedKeys(), (QStringList{loadKey, other}));

    /* Under a set with neither signal, both are carried; back under one
     * that has Engine Load, it is ticked again */
    const QStringList kept = model.plottedKeys();
    static ecuio_signal_def_t defs[1] = {};
    defs[0] = {};
    defs[0].id = 9001;
    std::strncpy(defs[0].name, "Lone", sizeof defs[0].name - 1);
    defs[0].message_id = 0x123;
    defs[0].bit_length = 8;
    defs[0].scale = 1.0;
    ecuio_signal_set_t lone{};
    lone.layout_version = ECUIO_SIGNAL_SET_LAYOUT;
    lone.defs = defs;
    lone.count = 1;
    model.setSignalSet(&lone);
    EXPECT_TRUE(model.plottedKeys().isEmpty()) << "a new set carries none";
    model.setPlotChoices(kept, {});
    EXPECT_EQ(model.plottedKeys(), kept) << "neither has a row here";
    EXPECT_TRUE(model.tickedIds().empty());
    model.setSignalSet(ecuio_obd2_signal_set());
    model.setPlotChoices(kept, {});
    EXPECT_TRUE(plotted(obd2RowOf(ECUIO_OBD2_SIG_ENGINE_LOAD_ID)))
        << "the carried tick rejoins its row";
    EXPECT_EQ(model.plottedKeys(), kept);

    /* At most CarriedMax in each list, the first: a save writes the rows'
     * keys before the carried, so the front is the newest */
    QStringList many;
    for (int i = 0; i < SignalListModel::CarriedMax + 6; ++i) {
        many << QStringLiteral("k%1").arg(i);
    }
    model.setPlotChoices(many, many);
    EXPECT_EQ(model.plottedKeys().size(), SignalListModel::CarriedMax);
    EXPECT_EQ(model.plottedKeys().first(), QStringLiteral("k0"));
    EXPECT_EQ(model.plottedKeys().last(),
              many[SignalListModel::CarriedMax - 1]);
    EXPECT_EQ(model.unplottedKeys(), model.plottedKeys());

    /* A full save re-read under a set that answers none of it keeps the
     * rows' keys of that save */
    model.setPlotChoices(QStringList{loadKey} + many, {});
    const QStringList saved = model.plottedKeys();
    ASSERT_EQ(saved.size(), SignalListModel::CarriedMax + 1);
    ASSERT_EQ(saved.first(), loadKey);
    model.setSignalSet(&lone);
    model.setPlotChoices(saved, {});
    EXPECT_EQ(model.plottedKeys().size(), SignalListModel::CarriedMax);
    EXPECT_EQ(model.plottedKeys().first(), loadKey)
        << "the latest session's tick is kept";
    model.setSignalSet(ecuio_obd2_signal_set());

    /* Another vehicle's carried texts are dropped, the rows' choices kept */
    model.setPlotChoices({loadKey, other}, {gone});
    model.dropCarriedChoices();
    EXPECT_EQ(model.plottedKeys(), QStringList{loadKey});
    EXPECT_TRUE(model.unplottedKeys().isEmpty());
}

/* ChoiceRows is core's find (ecuio_signal_set_find), answer for answer:
 * a key before another signal's name, a name one signal has, a name two
 * have, a signal with no key (a recording older than format 4 has names
 * alone), two of one key (the first), a signal with no name, which the
 * empty text doesn't find, and texts none has */
TEST(BridgeTest, ChoiceRowsAnswersAsCoresFindDoes)
{
    struct Sig {
        const char* key;
        const char* name;
    };
    const Sig sigs[] = {
        {"7E8:410C:2.0:16", "RPM"},
        {"7E9:410C:2.0:16", "RPM"},
        {"boost", "Boost A"},
        {"7E8:62D100:3.0:8", "boost"},
        {"", "Old"},
        {"", "Older"},
        {"7E8:4105:2.0:8", "Coolant"},
        {"7E8:4105:2.0:8", "Coolant again"},
        {"blank", ""},
    };
    const uint32_t n = static_cast<uint32_t>(std::size(sigs));
    std::vector<ecuio_signal_def_t> defs(n);
    std::vector<ecuio_signal_key_t> keys(n);
    ChoiceRows rows;
    for (uint32_t i = 0; i < n; ++i) {
        defs[i] = {};
        std::strncpy(defs[i].name, sigs[i].name, sizeof defs[i].name - 1);
        keys[i] = {};
        std::strncpy(keys[i].text, sigs[i].key, sizeof keys[i].text - 1);
        rows.add(QByteArray(sigs[i].key), QByteArray(sigs[i].name));
    }
    ecuio_signal_set_t set{};
    set.defs = defs.data();
    set.count = n;

    std::vector<std::string> texts = {"", "nothing", "7E8", "rpm", "Boost"};
    for (const Sig& s : sigs) {
        texts.emplace_back(s.key);
        texts.emplace_back(s.name);
    }
    for (const std::string& text : texts) {
        uint32_t index = 99;
        const ecuio_status_t st =
            ecuio_signal_set_find(&set, keys.data(), n, text.c_str(), &index);
        EXPECT_EQ(rows.rowOf(QByteArray::fromStdString(text)),
                  st == ECUIO_OK ? static_cast<int>(index) : -1)
            << "'" << text << "'";
    }
    EXPECT_EQ(rows.rowOf("boost"), 2) << "a key before another's name";
    EXPECT_EQ(rows.rowOf("Boost A"), 2);
    EXPECT_EQ(rows.rowOf("RPM"), -1) << "a name two signals have";
    EXPECT_EQ(rows.rowOf("7E9:410C:2.0:16"), 1);
    EXPECT_EQ(rows.rowOf("Old"), 4) << "with no key, by its name";
    EXPECT_EQ(rows.rowOf(""), -1) << "never the signal with no name";
    EXPECT_EQ(rows.rowOf("7E8:4105:2.0:8"), 6) << "the first of two";
}
