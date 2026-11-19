// File/layer: tests/test_bridge_pluginpages.cpp | GTest + QtCore, app-layer tests
// Testing: BRIDGE: BridgeTest.OneStoreKeepsAPluginsPages, .OneStoreAPutKeepsThePages,
//          .OneStoreAnUnknownPageIsEmpty (test_bridge)
//
// The Dashboard pages a plugin carries (ADR-0052's appendix on pages), as
// the store lists them (plugins()' pages) and hands out their text
// (pluginPageText): core's text, through an import, a later start, a put
// and a drop.

#include <gtest/gtest.h>

#include <QByteArray>
#include <QString>
#include <QTemporaryDir>
#include <QVariantList>
#include <QVariantMap>

#include <ecuio/core.h>
#include <ecuio/plugin.h>

#include "packed_plugin.h"
#include "pluginstore.h"
#include "sessionbridge.h"
#include "bridge_support.h"

namespace {

const QString kId = QStringLiteral("paged");

QByteArray page(const char* form, const char* name)
{
    return QByteArray(R"({"format": 2, "form": ")") + form +
           R"(", "pages": [{"name": ")" + name +
           R"(", "cells": [{"kind": "digital", "w": 3, "h": 9, "label": "Oil",)"
           R"( "refs": ["key:oil"]}]}]})";
}

/* A packed plugin of one variant with a phone page, then a desktop one */
QByteArray paged()
{
    return QByteArray(
               R"({"havne_plugin": 1, "id": "paged", "name": "Paged", "version": "1.0.0",)"
               R"( "requires": {"core": "0.1.0"}, "source": {"name": "t"}, "tiers": ["data"],)"
               R"( "ecus": [{"id": "pcm", "rx": "7E8"}],)"
               R"( "variants": [{"id": "base", "files": ["data/base.json"]}],)"
               R"( "pages": ["pages/phone.json", "pages/desktop.json"],)"
               R"( "files": {"data/base.json": {"havne_signals": 1, "signals": [{"key": "oil",)"
               R"( "name": "Oil", "ecu": "pcm", "read": {"service": "22", "number": "F40D"},)"
               R"( "field": {"byte": 0}}]}, "pages/phone.json": )") +
           page("phone", "Engine") + R"(, "pages/desktop.json": )" +
           page("desktop", "Engine desk") + "}}";
}

/* Core's own text of each page of bytes */
QStringList coresTexts(const QByteArray& bytes)
{
    ecuio_plugin_t* p = nullptr;
    EXPECT_EQ(ecuio_plugin_load(bytes.constData(),
                                static_cast<size_t>(bytes.size()), nullptr,
                                nullptr, &p),
              ECUIO_OK)
        << ecuio_last_error_detail();
    QStringList out;
    for (uint32_t i = 0; p != nullptr && ecuio_plugin_page_text(p, i); ++i) {
        out << QString::fromUtf8(ecuio_plugin_page_text(p, i));
    }
    ecuio_plugin_free(p);
    return out;
}

QVariantMap entryWithId(SessionBridge& bridge, const QString& id)
{
    for (const QVariant& v : bridge.plugins()) {
        if (v.toMap().value(QStringLiteral("id")).toString() == id) {
            return v.toMap();
        }
    }
    return {};
}

QVariantList expectedPages()
{
    return {QVariantMap{
                {QStringLiteral("index"), 0},
                {QStringLiteral("form"), QStringLiteral("phone")},
                {QStringLiteral("path"), QStringLiteral("pages/phone.json")}},
            QVariantMap{{QStringLiteral("index"), 1},
                        {QStringLiteral("form"), QStringLiteral("desktop")},
                        {QStringLiteral("path"),
                         QStringLiteral("pages/desktop.json")}}};
}

bool ok(const QVariantMap& r)
{
    return r.value(QStringLiteral("ok")).toBool();
}

std::string words(const QVariantMap& r)
{
    return r.value(QStringLiteral("words")).toString().toStdString();
}

} // namespace

/* An imported plugin's entry lists its pages in the manifest's order, a
 * plugin without pages an empty list, and each page's text is core's;
 * a later start reads them from the kept copy */
TEST(BridgeTest, OneStoreKeepsAPluginsPages)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir from;
    const QStringList texts = coresTexts(paged());
    ASSERT_EQ(texts.size(), 2);
    {
        SessionBridge bridge;
        QVariantMap r = bridge.importPlugin(
            writeScratch(from.path(), QStringLiteral("p.json"), paged()));
        ASSERT_TRUE(ok(r)) << words(r);
        r = bridge.importPlugin(writeScratch(
            from.path(), QStringLiteral("e.json"), packed_plugin::example()));
        ASSERT_TRUE(ok(r)) << words(r);
        EXPECT_EQ(
            entryWithId(bridge, kId).value(QStringLiteral("pages")).toList(),
            expectedPages());
        const QVariantMap none =
            entryWithId(bridge, QStringLiteral("example-sim"));
        ASSERT_TRUE(none.contains(QStringLiteral("pages")));
        EXPECT_TRUE(none.value(QStringLiteral("pages")).toList().isEmpty());
        EXPECT_EQ(bridge.pluginPageText(kId, 0), texts.at(0));
        EXPECT_EQ(bridge.pluginPageText(kId, 1), texts.at(1));
    }
    SessionBridge later;
    EXPECT_EQ(later.pluginPageText(kId, 1), texts.at(1)) << "no list first";
    EXPECT_EQ(entryWithId(later, kId).value(QStringLiteral("pages")).toList(),
              expectedPages());
}

/* A plugin of the user's own keeps its pages through a variant put in and
 * one dropped */
TEST(BridgeTest, OneStoreAPutKeepsThePages)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir from;
    const QStringList texts = coresTexts(paged());
    ASSERT_EQ(texts.size(), 2);
    SessionBridge bridge;
    QVariantMap r = bridge.importPlugin(
        writeScratch(from.path(), QStringLiteral("p.json"), paged()));
    ASSERT_TRUE(ok(r)) << words(r);
    const QString sha =
        entryWithId(bridge, kId).value(QStringLiteral("sha256")).toString();
    ASSERT_TRUE(ok(bridge.markPluginMine(sha)));

    r = bridge.readImport(writeScratch(
        from.path(), QStringLiteral("extra.json"),
        signalFile(QStringLiteral("Extra"), QStringLiteral("D150"))));
    ASSERT_TRUE(ok(r)) << words(r);
    r = bridge.keepSignals(sha, QStringLiteral("extra"), QString());
    ASSERT_TRUE(ok(r)) << words(r);
    QVariantMap e = entryWithId(bridge, kId);
    const QString put = e.value(QStringLiteral("sha256")).toString();
    ASSERT_NE(put, sha) << "the put made a new copy";
    EXPECT_EQ(e.value(QStringLiteral("variants")).toStringList(),
              (QStringList{QStringLiteral("base"), QStringLiteral("extra")}));
    EXPECT_EQ(e.value(QStringLiteral("pages")).toList(), expectedPages());
    EXPECT_EQ(bridge.pluginPageText(kId, 0), texts.at(0));
    EXPECT_EQ(bridge.pluginPageText(kId, 1), texts.at(1));

    r = bridge.dropPluginVariant(put, QStringLiteral("extra"));
    ASSERT_TRUE(ok(r)) << words(r);
    e = entryWithId(bridge, kId);
    EXPECT_EQ(e.value(QStringLiteral("variants")).toStringList(),
              QStringList{QStringLiteral("base")});
    EXPECT_EQ(e.value(QStringLiteral("pages")).toList(), expectedPages());
    EXPECT_EQ(bridge.pluginPageText(kId, 1), texts.at(1));
}

/* No such plugin, an index past the pages or below them, and a plugin
 * removed: each is "" */
TEST(BridgeTest, OneStoreAnUnknownPageIsEmpty)
{
    SettingsDirScope dir;
    ASSERT_TRUE(dir.valid());
    QTemporaryDir from;
    SessionBridge bridge;
    EXPECT_EQ(bridge.pluginPageText(kId, 0), QString()) << "an empty store";
    const QVariantMap r = bridge.importPlugin(
        writeScratch(from.path(), QStringLiteral("p.json"), paged()));
    ASSERT_TRUE(ok(r)) << words(r);
    ASSERT_FALSE(bridge.pluginPageText(kId, 1).isEmpty());
    EXPECT_EQ(bridge.pluginPageText(kId, 2), QString());
    EXPECT_EQ(bridge.pluginPageText(kId, -1), QString());
    EXPECT_EQ(bridge.pluginPageText(QStringLiteral("other"), 0), QString());
    EXPECT_EQ(bridge.pluginPageText(kId.toUpper(), 0), QString());
    const QString sha =
        entryWithId(bridge, kId).value(QStringLiteral("sha256")).toString();
    ASSERT_TRUE(ok(bridge.removePlugin(sha)));
    EXPECT_EQ(bridge.pluginPageText(kId, 0), QString());
}
