#include <gtest/gtest.h>

#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

#include <memory>
#include <string>
#include <vector>

#include "sessionbridge.h"
#include <ecuio/signals.h>
#include "bridge_support.h"

namespace {

using File =
    std::unique_ptr<ecuio_signal_file_t, void (*)(ecuio_signal_file_t*)>;

File load(const std::string& json)
{
    ecuio_signal_file_t* f = nullptr;
    EXPECT_EQ(ecuio_signal_file_load(json.data(), json.size(), &f), ECUIO_OK)
        << ecuio_last_error_detail();
    return File(f, ecuio_signal_file_free);
}

std::string sig(const char* key, const char* name, const char* ecu,
                const char* did)
{
    return std::string(R"({"key":")") + key + R"(","name":")" + name +
           R"(","ecu":")" + ecu + R"(","read":{"service":"22","number":")" +
           did + R"("},"field":{"byte":0}})";
}

// Engine (7E8) and Body (7EA), each with a Battery voltage; one Fuel level
const std::string kTwoModules =
    R"({"havne_signals":3,"name":"Two","source":{"name":"t"},"ecus":[)"
    R"({"id":"pcm","rx":"7E8","name":"Engine"},{"id":"bcm","rx":"7EA","name":"Body"}],)"
    R"("signals":[)" +
    sig("ev", "Battery voltage", "pcm", "D100") + "," +
    sig("bv", "Battery voltage", "bcm", "D100") + "," +
    sig("fuel", "Fuel level", "bcm", "D101") + "," +
    R"({"key":"sum","name":"Sum","kind":"computed","expr":"ev + bv"},)"
    R"({"key":"twice","name":"Twice","kind":"computed","expr":"ev * 2"}]})";

std::string onePart(const char* name, const char* did)
{
    return std::string(R"({"havne_signals":1,"name":")") + name +
           R"(","source":{"name":"t"},"ecus":[{"id":"pcm","rx":"7E8"}],)"
           R"("signals":[{"key":"oil_)" +
           QString::fromUtf8(did).toLower().toStdString() +
           R"(","name":"Oil temperature","read":{"service":"22","number":")" +
           did + R"("},"field":{"byte":0}}]})";
}

QString at(const SignalListModel& m, int row, int role)
{
    return m.data(m.index(row), role).toString();
}

// kTwoModules' signals by their place in the set
const QStringList kKeys{"ev", "bv", "fuel", "sum", "twice"};

int rowNamed(const SignalListModel&, const QString& key)
{
    return static_cast<int>(kKeys.indexOf(key));
}

QStringList shownKeys(const LiveSignalModel& live)
{
    QStringList out;
    for (int r = 0; r < live.rowCount(); ++r) {
        out << kKeys.value(
            live.data(live.index(r, 0), SignalListModel::RowRole).toInt());
    }
    return out;
}

} // namespace

TEST(BridgeTest, ModulesTagARepeatedNameByModuleThenPartThenKey)
{
    {
        const File f = load(kTwoModules);
        SignalListModel m;
        m.setSignalSet(ecuio_signal_file_set(f.get()), f.get());
        EXPECT_EQ(at(m, rowNamed(m, "ev"), SignalListModel::TagRole), "Engine");
        EXPECT_EQ(at(m, rowNamed(m, "bv"), SignalListModel::TagRole), "Body");
    }
    {
        const File a = load(onePart("Alpha", "D100"));
        const File b = load(onePart("Beta", "D200"));
        const ecuio_signal_file_t* parts[] = {a.get(), b.get()};
        ecuio_signal_file_t* made = nullptr;
        ASSERT_EQ(ecuio_signal_file_merge(parts, 2, nullptr, &made), ECUIO_OK)
            << ecuio_last_error_detail();
        const File merged(made, ecuio_signal_file_free);
        SignalListModel m;
        m.setSignalSet(ecuio_signal_file_set(merged.get()), merged.get());
        ASSERT_EQ(m.rowCount(), 2);
        EXPECT_EQ(at(m, 0, SignalListModel::TagRole), "Alpha")
            << "one module: the part tells them apart";
        EXPECT_EQ(at(m, 1, SignalListModel::TagRole), "Beta");
    }
    {
        const File f =
            load(R"({"havne_signals":3,"name":"One","source":{"name":"t"},)"
                 R"("ecus":[{"id":"pcm","rx":"7E8"}],"signals":[)" +
                 sig("x", "Same", "pcm", "D100") + "," +
                 sig("y", "Same", "pcm", "D101") + "]}");
        SignalListModel m;
        m.setSignalSet(ecuio_signal_file_set(f.get()), f.get());
        EXPECT_EQ(at(m, 0, SignalListModel::TagRole),
                  at(m, 0, SignalListModel::KeyRole))
            << "one module, one part: the key";
        EXPECT_NE(at(m, 0, SignalListModel::TagRole),
                  at(m, 1, SignalListModel::TagRole));
    }
}

TEST(BridgeTest, ModulesTagNothingForANameOneRowHas)
{
    const File f = load(kTwoModules);
    SignalListModel m;
    m.setSignalSet(ecuio_signal_file_set(f.get()), f.get());
    EXPECT_EQ(at(m, rowNamed(m, "fuel"), SignalListModel::TagRole), "");
    EXPECT_EQ(at(m, rowNamed(m, "fuel"), SignalListModel::ModuleNameRole),
              "Body");
}

TEST(BridgeTest, ModulesTagOverTheSetsRowsNotTheFilteredOnes)
{
    const File f = load(kTwoModules);
    SignalListModel m;
    m.setSignalSet(ecuio_signal_file_set(f.get()), f.get());
    LiveSignalModel live(&m);
    live.filter()->toggleModule(0);
    ASSERT_EQ(shownKeys(live).count(QStringLiteral("ev")), 1);
    const int r =
        static_cast<int>(shownKeys(live).indexOf(QStringLiteral("ev")));
    EXPECT_EQ(live.data(live.index(r, 0), SignalListModel::TagRole).toString(),
              "Engine")
        << "the other Battery voltage is filtered out, still told apart";
}

TEST(BridgeTest, ModulesFilterPassesARowOfAChosenModuleAndNoneElse)
{
    const File f = load(kTwoModules);
    SignalListModel m;
    m.setSignalSet(ecuio_signal_file_set(f.get()), f.get());
    EXPECT_EQ(m.data(m.index(rowNamed(m, "sum")), SignalListModel::ModuleRole)
                  .toInt(),
              -1)
        << "leaves on two modules";
    EXPECT_EQ(m.data(m.index(rowNamed(m, "twice")), SignalListModel::ModuleRole)
                  .toInt(),
              0)
        << "a computed row takes its leaves' module";
    LiveSignalModel live(&m);
    EXPECT_EQ(live.rowCount(), 5);
    live.filter()->toggleModule(1);
    EXPECT_TRUE(live.filter()->active());
    EXPECT_EQ(shownKeys(live), (QStringList{"bv", "fuel"}))
        << "no module-less row under a chosen module";
    live.filter()->toggleModule(0);
    EXPECT_EQ(shownKeys(live), (QStringList{"ev", "bv", "fuel", "twice"}));
    live.filter()->clear();
    EXPECT_EQ(live.rowCount(), 5);
    EXPECT_FALSE(live.filter()->active());
}

TEST(BridgeTest, ModulesChoicesNameEachModuleWithAShownRowOnce)
{
    const File f = load(kTwoModules);
    SignalListModel m;
    m.setSignalSet(ecuio_signal_file_set(f.get()), f.get());
    LiveSignalModel live(&m);
    const QVariantList both{QVariantMap{{QStringLiteral("index"), 0},
                                        {QStringLiteral("name"), "Engine"}},
                            QVariantMap{{QStringLiteral("index"), 1},
                                        {QStringLiteral("name"), "Body"}}};
    EXPECT_EQ(live.modules(), both);
    const ecuio_signal_set_t* set = ecuio_signal_file_set(f.get());
    m.applyUnsupported(
        {set->defs[rowNamed(m, "bv")].id, set->defs[rowNamed(m, "fuel")].id});
    EXPECT_EQ(live.modules(), QVariantList{both.first()})
        << "a module with only hidden rows isn't offered";
}

TEST(BridgeTest, ModulesProfileSignalsGiveEachRowItsModuleAndTag)
{
    SettingsDirScope dir;
    SessionBridge bridge;
    const QVariantMap a = bridge.keepSignalText(
        QString::fromStdString(onePart("Alpha", "D100")), {}, {}, {});
    const QVariantMap b = bridge.keepSignalText(
        QString::fromStdString(onePart("Beta", "D200")), {}, {}, {});
    const QVariantMap set = bridge.vehicleSet(
        {a.value(QStringLiteral("profiles")).toStringList().value(0),
         b.value(QStringLiteral("profiles")).toStringList().value(0)},
        {});
    ASSERT_TRUE(set.value(QStringLiteral("ok")).toBool());
    const QVariantList rows =
        bridge.profileSignals(QStringLiteral("mock://t?profile=") +
                              set.value(QStringLiteral("profile")).toString());
    ASSERT_EQ(rows.size(), 2);
    EXPECT_EQ(rows.at(0).toMap().value(QStringLiteral("module")).toInt(), 0);
    EXPECT_EQ(rows.at(0).toMap().value(QStringLiteral("moduleName")).toString(),
              "PCM");
    EXPECT_EQ(rows.at(0).toMap().value(QStringLiteral("tag")).toString(),
              "Alpha");
    EXPECT_EQ(rows.at(1).toMap().value(QStringLiteral("tag")).toString(),
              "Beta");
    removeEveryPlugin(bridge);
}
