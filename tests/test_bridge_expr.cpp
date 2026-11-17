// File/layer: tests/test_bridge_expr.cpp | GTest + QtCore, app-layer tests
// Testing: BRIDGE: BridgeTest.Expr* (test_bridge)
//
// The signal form's computed-signal editor (app/exprcheck.h): core's
// answers marshalled to plain values. Session-free; the judgments
// themselves are core's (SignalExprTest).

#include <gtest/gtest.h>

#include <QString>
#include <QVariantList>
#include <QVariantMap>

#include <cmath>
#include <limits>

#include "exprcheck.h"

namespace {

// A file extending obd2 with a number, a text, a computed signal over
// the number and a number whose key starts with a digit
const char kFile[] =
    R"({"havne_signals":2,"name":"Expr","extends":"obd2",
        "source":{"name":"t"},"ecus":[{"id":"pcm","rx":"7E8"}],
        "signals":[
          {"key":"oil","name":"Oil","unit":"kPa",
           "read":{"service":"22","number":"F40D"},"field":{"byte":0}},
          {"key":"2nd","name":"Second","unit":"V",
           "read":{"service":"22","number":"F40F"},"field":{"byte":0}},
          {"key":"label","name":"Label","kind":"text",
           "read":{"service":"22","number":"F40E"},"field":{"byte":0}},
          {"key":"twice","name":"Twice","kind":"computed","expr":"oil * 2"}]})";

QVariantMap value(const char* ref, double v, double ageMs = 0)
{
    return {{QStringLiteral("ref"), QString::fromLatin1(ref)},
            {QStringLiteral("value"), v},
            {QStringLiteral("ageMs"), ageMs}};
}

QVariantMap rowOf(const QVariantList& rows, const QString& name)
{
    for (const QVariant& r : rows) {
        if (r.toMap().value(QStringLiteral("name")).toString() == name) {
            return r.toMap();
        }
    }
    ADD_FAILURE() << "no row named " << name.toStdString();
    return {};
}

ExprEditor& loaded(ExprEditor& e, const char* json = kFile)
{
    const QVariantMap r = e.setFile(QString::fromUtf8(json));
    EXPECT_TRUE(r.value(QStringLiteral("ok")).toBool()) << r.value(QStringLiteral("words")).toString().toStdString();
    return e;
}

} // namespace

TEST(BridgeTest, ExprCheckAcceptsWhatItNamesAndListsWhatItReads)
{
    ExprEditor e;
    loaded(e);
    const QVariantMap r = e.check(QString(), QStringLiteral("oil + obd2.rpm"));
    ASSERT_TRUE(r.value(QStringLiteral("ok")).toBool()) << r.value(QStringLiteral("words")).toString().toStdString();
    EXPECT_EQ(r.value(QStringLiteral("at")).toInt(), 0);
    const QVariantList leaves = r.value(QStringLiteral("leaves")).toList();
    ASSERT_EQ(leaves.size(), 2);
    EXPECT_EQ(leaves[0].toMap().value(QStringLiteral("ref")).toString(), QStringLiteral("oil"));
    EXPECT_EQ(leaves[0].toMap().value(QStringLiteral("unit")).toString(), QStringLiteral("kPa"));
    EXPECT_EQ(leaves[1].toMap().value(QStringLiteral("ref")).toString(), QStringLiteral("obd2.rpm"));
}

TEST(BridgeTest, ExprCheckPlacesARefusalFromOne)
{
    ExprEditor e;
    loaded(e);
    const QVariantMap r = e.check(QString(), QStringLiteral("oil + bost"));
    EXPECT_FALSE(r.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(r.value(QStringLiteral("at")).toInt(), 7);
    EXPECT_TRUE(r.value(QStringLiteral("words")).toString().contains(QStringLiteral("bost")));
    EXPECT_EQ(e.check(QString(), QStringLiteral("")).value(QStringLiteral("at")).toInt(), 1);
}

TEST(BridgeTest, ExprAnOverlongExpressionIsRefusedAtOne)
{
    ExprEditor e;
    loaded(e);
    const QString ok(QStringLiteral("oil")), pad = QString(253, QLatin1Char(' '));
    EXPECT_TRUE(e.check(QString(), ok + pad).value(QStringLiteral("ok")).toBool());
    const QVariantMap r = e.check(QString(), ok + pad + QStringLiteral(" "));
    EXPECT_FALSE(r.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(r.value(QStringLiteral("at")).toInt(), 1);
}

TEST(BridgeTest, ExprAnEditsOwnKeyIsJudgedInItsPlace)
{
    ExprEditor e;
    loaded(e);
    // A circle through the signal being edited is refused; as a new one it isn't
    EXPECT_FALSE(e.check(QStringLiteral("twice"), QStringLiteral("twice + 1")).value(QStringLiteral("ok")).toBool());
    EXPECT_TRUE(e.check(QString(), QStringLiteral("twice + 1")).value(QStringLiteral("ok")).toBool());
}

TEST(BridgeTest, ExprFormatIsReportedForAFunction)
{
    ExprEditor e;
    loaded(e);
    EXPECT_EQ(e.check(QString(), QStringLiteral("oil - 1")).value(QStringLiteral("format")).toInt(), 1);
    EXPECT_EQ(e.check(QString(), QStringLiteral("max(oil, 1)")).value(QStringLiteral("format")).toInt(), 2);
}

TEST(BridgeTest, ExprNamesAreWhatAnExpressionReadsThemBy)
{
    ExprEditor e;
    loaded(e);
    const QVariantList rows = e.names();
    EXPECT_GT(rows.size(), 50);   // obd2's too
    const QVariantMap oil = rowOf(rows, QStringLiteral("Oil"));
    EXPECT_EQ(oil.value(QStringLiteral("ref")).toString(), QStringLiteral("oil"));
    EXPECT_TRUE(oil.value(QStringLiteral("usable")).toBool());
    // Every usable row's name compiles
    int checked = 0;
    for (const QVariant& r : rows) {
        const QVariantMap m = r.toMap();
        if (!m.value(QStringLiteral("usable")).toBool()) {
            continue;
        }
        const QVariantMap c = e.check(QString(), m.value(QStringLiteral("ref")).toString());
        EXPECT_TRUE(c.value(QStringLiteral("ok")).toBool())
            << m.value(QStringLiteral("ref")).toString().toStdString() << ": "
            << c.value(QStringLiteral("words")).toString().toStdString();
        ++checked;
    }
    EXPECT_GT(checked, 50);
}

TEST(BridgeTest, ExprADigitFirstKeyAndATextRowAreNotUsable)
{
    ExprEditor e;
    loaded(e);
    const QVariantList rows = e.names();
    const QVariantMap second = rowOf(rows, QStringLiteral("Second"));
    EXPECT_FALSE(second.value(QStringLiteral("usable")).toBool());
    EXPECT_TRUE(second.value(QStringLiteral("why")).toString().contains(QStringLiteral("digit")))
        << second.value(QStringLiteral("why")).toString().toStdString();
    const QVariantMap label = rowOf(rows, QStringLiteral("Label"));
    EXPECT_FALSE(label.value(QStringLiteral("usable")).toBool());
    EXPECT_FALSE(label.value(QStringLiteral("why")).toString().isEmpty());
}

TEST(BridgeTest, ExprLeavesGoThroughAComputedInputOnceEach)
{
    ExprEditor e;
    loaded(e);
    const QVariantMap r = e.check(QString(), QStringLiteral("twice + oil"));
    ASSERT_TRUE(r.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(r.value(QStringLiteral("inputs")).toList().size(), 2);
    const QVariantList leaves = r.value(QStringLiteral("leaves")).toList();
    ASSERT_EQ(leaves.size(), 1);   // oil, under twice and by itself
    EXPECT_EQ(leaves[0].toMap().value(QStringLiteral("ref")).toString(), QStringLiteral("oil"));
}

TEST(BridgeTest, ExprEvaluateWorksTheValueOutByRef)
{
    ExprEditor e;
    loaded(e);
    ASSERT_TRUE(e.check(QString(), QStringLiteral("oil - obd2.rpm / 100")).value(QStringLiteral("ok")).toBool());
    const QVariantMap r = e.evaluate({value("oil", 300), value("obd2.rpm", 2000)});
    ASSERT_TRUE(r.value(QStringLiteral("ok")).toBool());
    EXPECT_TRUE(r.value(QStringLiteral("hasValue")).toBool());
    EXPECT_DOUBLE_EQ(r.value(QStringLiteral("value")).toDouble(), 280.0);
    EXPECT_EQ(r.value(QStringLiteral("why")).toString(), QString());
}

TEST(BridgeTest, ExprEvaluateNamesTheMissingLeaf)
{
    ExprEditor e;
    loaded(e);
    ASSERT_TRUE(e.check(QString(), QStringLiteral("oil - obd2.rpm")).value(QStringLiteral("ok")).toBool());
    const QVariantMap r = e.evaluate({value("oil", 1)});
    EXPECT_FALSE(r.value(QStringLiteral("hasValue")).toBool());
    EXPECT_EQ(r.value(QStringLiteral("why")).toString(), QStringLiteral("missing"));
    EXPECT_EQ(r.value(QStringLiteral("leafName")).toString(), QStringLiteral("RPM"))
        << r.value(QStringLiteral("leafName")).toString().toStdString();
}

TEST(BridgeTest, ExprEvaluateSaysStaleForAnAgedValue)
{
    ExprEditor e;
    loaded(e);
    ASSERT_TRUE(e.check(QString(), QStringLiteral("oil + 1")).value(QStringLiteral("ok")).toBool());
    const QVariantMap r = e.evaluate({value("oil", 1, 10000)});   // past the 3 s default
    EXPECT_EQ(r.value(QStringLiteral("why")).toString(), QStringLiteral("stale"));
    EXPECT_GT(r.value(QStringLiteral("boundMs")).toDouble(), 0.0);
}

TEST(BridgeTest, ExprEvaluateSaysNoNumberForADivisionByZero)
{
    ExprEditor e;
    loaded(e);
    ASSERT_TRUE(e.check(QString(), QStringLiteral("oil / obd2.rpm")).value(QStringLiteral("ok")).toBool());
    const QVariantMap r = e.evaluate({value("oil", 1), value("obd2.rpm", 0)});
    EXPECT_FALSE(r.value(QStringLiteral("hasValue")).toBool());
    EXPECT_EQ(r.value(QStringLiteral("why")).toString(), QStringLiteral("noNumber"));
}

TEST(BridgeTest, ExprEvaluateLeavesOutAnUnknownRefAndANonFiniteValue)
{
    ExprEditor e;
    loaded(e);
    ASSERT_TRUE(e.check(QString(), QStringLiteral("oil + 1")).value(QStringLiteral("ok")).toBool());
    // An unknown ref is passed by; a NaN or infinite value is left out, so oil is missing
    EXPECT_EQ(e.evaluate({value("nope", 1), value("oil", std::nan(""))}).value(QStringLiteral("why")).toString(),
              QStringLiteral("missing"));
    EXPECT_EQ(e.evaluate({value("oil", std::numeric_limits<double>::infinity())}).value(QStringLiteral("why")).toString(),
              QStringLiteral("missing"));
    EXPECT_TRUE(e.evaluate({value("nope", 1), value("oil", 2)}).value(QStringLiteral("hasValue")).toBool());
}

TEST(BridgeTest, ExprWithNoFileSaysSoAndNeverCrashes)
{
    ExprEditor e;
    EXPECT_FALSE(e.check(QString(), QStringLiteral("1 + 1")).value(QStringLiteral("ok")).toBool());
    EXPECT_FALSE(e.evaluate({}).value(QStringLiteral("ok")).toBool());
    EXPECT_TRUE(e.names().isEmpty());
    // A refused file leaves it with none
    const QVariantMap r = e.setFile(QStringLiteral("{not json"));
    EXPECT_FALSE(r.value(QStringLiteral("ok")).toBool());
    EXPECT_FALSE(r.value(QStringLiteral("words")).toString().isEmpty());
    EXPECT_FALSE(e.check(QString(), QStringLiteral("1 + 1")).value(QStringLiteral("ok")).toBool());
}

TEST(BridgeTest, ExprACheckBeforeAnEvaluateIsRequiredAndARefusalForgetsTheLast)
{
    ExprEditor e;
    loaded(e);
    ASSERT_TRUE(e.check(QString(), QStringLiteral("oil + 1")).value(QStringLiteral("ok")).toBool());
    EXPECT_TRUE(e.evaluate({value("oil", 1)}).value(QStringLiteral("hasValue")).toBool());
    EXPECT_FALSE(e.check(QString(), QStringLiteral("bost")).value(QStringLiteral("ok")).toBool());
    EXPECT_FALSE(e.evaluate({value("oil", 1)}).value(QStringLiteral("ok")).toBool());
}

TEST(BridgeTest, ExprSettingAFileTwiceReplacesTheFirstAndTwoEditorsAreApart)
{
    ExprEditor a, b;
    loaded(a);
    loaded(b, R"({"havne_signals":1,"name":"Other","source":{"name":"t"},"ecus":[{"id":"pcm","rx":"7E8"}],
        "signals":[{"key":"gear","name":"Gear","read":{"service":"22","number":"F40D"},"field":{"byte":0}}]})");
    loaded(a);   // the first file is freed (the sanitizers' case)
    EXPECT_TRUE(a.check(QString(), QStringLiteral("oil + 1")).value(QStringLiteral("ok")).toBool());
    EXPECT_FALSE(b.check(QString(), QStringLiteral("oil + 1")).value(QStringLiteral("ok")).toBool());
}

TEST(BridgeTest, ExprAnAgeBeyondTheRangeStillEvaluates)
{
    ExprEditor e;
    loaded(e);
    ASSERT_TRUE(e.check(QString(), QStringLiteral("oil + 1")).value(QStringLiteral("ok")).toBool());
    const QVariantMap r = e.evaluate({value("oil", 1, 1e300)});
    EXPECT_TRUE(r.value(QStringLiteral("ok")).toBool());
    EXPECT_FALSE(r.value(QStringLiteral("hasValue")).toBool());
    EXPECT_EQ(r.value(QStringLiteral("why")).toString(), QStringLiteral("stale"));
}
