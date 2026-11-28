#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QStringList>
#include <QTemporaryDir>

#include "sessionbridge.h"
#include "bridge_support.h"

namespace {

ecuio_module_codes_t answered()
{
    ecuio_module_codes_t r{};
    r.flags = ECUIO_MODULE_CODES_ASKABLE | ECUIO_MODULE_CODES_COMPLETE |
              ECUIO_MODULE_CODES_FORMAT;
    r.rx_id = 0x7E8;
    r.read_id = 1;
    r.count_outcome = ECUIO_DIAG_ANSWERED;
    r.list_outcome = ECUIO_DIAG_ANSWERED;
    r.held = r.reported = 3;
    r.codes[0] = {0x012800, 0x20, {}};
    r.codes[1] = {0x030100, 0x08, {}};
    r.codes[2] = {0x9A011C, 0x89, {}};
    return r;
}

} // namespace

TEST(BridgeTest, TheReadVisitsEveryModuleInOrderAndLogsOnce)
{
    SessionBridge bridge;
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    const auto lines = [&log]() {
        return log.filter(QStringLiteral("ECU 0x7E8 enhanced codes: "));
    };
    ASSERT_TRUE(bridge.connectSession(QStringLiteral("mock://demo")).isEmpty());
    bridge.readModuleCodes();
    ASSERT_TRUE(spinUntil([&]() { return lines().size() == 1; }, 4000))
        << log.join('\n').toStdString();
    spinLoop(400);
    ASSERT_EQ(lines().size(), 1);
    EXPECT_EQ(lines().first().toStdString(),
              "ECU 0x7E8 enhanced codes: P0128-00 not now (history), "
              "U1123-45 not now (pending), B1A01-1C active (stored, "
              "warning lamp requested), P0301-00 not now (stored, also an "
              "emissions code)");
    const QVariantMap e =
        bridge.diagnostics().at(0).toMap().value("enhanced").toMap();
    EXPECT_EQ(e.value("outcome").toString(), QStringLiteral("answered"));
    EXPECT_EQ(e.value("codes").toStringList().size(), 4);
    bridge.readDiagnostics();
    ASSERT_TRUE(spinUntil([&]() { return lines().size() == 2; }, 4000))
        << "Read again is the same pass";
    ASSERT_TRUE(disconnectAndWait(bridge));
}

TEST(BridgeTest, AnUnansweredModuleReadsNoResponse)
{
    ecuio_module_codes_t r{};
    r.flags = ECUIO_MODULE_CODES_ASKABLE | ECUIO_MODULE_CODES_COMPLETE;
    r.rx_id = 0x7E9;
    r.read_id = 1;
    r.count_outcome = ECUIO_DIAG_UNANSWERED;
    r.list_outcome = ECUIO_DIAG_UNANSWERED;
    const ecuio_channel_diagnostics_t standard{};
    EXPECT_EQ(BridgeFormats::moduleCodesEntry(r, standard, false)
                  .value("outcome")
                  .toString(),
              QStringLiteral("no_response"));
    EXPECT_EQ(BridgeFormats::moduleCodesText(r, standard, false).toStdString(),
              "ECU 0x7E9 enhanced codes: no response");
    EXPECT_EQ(
        BridgeFormats::moduleCodesEntry(ecuio_module_codes_t{}, standard, false)
            .value("outcome")
            .toString(),
        QStringLiteral("not_read"));
}

TEST(BridgeTest, AModuleThatReadsItsCodesOnlyInTheSessionSaysSo)
{
    ecuio_module_codes_t r{};
    r.flags = ECUIO_MODULE_CODES_ASKABLE | ECUIO_MODULE_CODES_COMPLETE;
    r.rx_id = 0x7E9;
    r.read_id = 1;
    r.count_outcome = ECUIO_DIAG_REFUSED;
    r.list_outcome = ECUIO_DIAG_REFUSED;
    r.list_nrc = 0x7F;
    const ecuio_channel_diagnostics_t standard{};
    EXPECT_EQ(BridgeFormats::moduleCodesText(r, standard, false).toStdString(),
              "ECU 0x7E9 enhanced codes: refused (7F 19 7F): the module reads "
              "its codes only in its extended session");
    EXPECT_TRUE(BridgeFormats::moduleCodesText(r, standard, true)
                    .endsWith(QStringLiteral(", connect with ?session=03")));
}

TEST(BridgeTest, AClearsSavedFileHoldsTheEnhancedCodes)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    SessionBridge bridge;
    bridge.setRecordingsRootForTesting(root.path());
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    ASSERT_TRUE(bridge.connectSession(QStringLiteral("mock://demo")).isEmpty());
    bridge.readModuleCodes();
    ASSERT_TRUE(spinUntil(
        [&]() {
            return !log.filter(QStringLiteral("enhanced codes: ")).isEmpty();
        },
        4000));
    ASSERT_FALSE(bridge.clearDiagnostics().isEmpty());
    const QStringList files =
        QDir(root.path()).entryList({QStringLiteral("*.jsonl")}, QDir::Files);
    ASSERT_EQ(files.size(), 1);
    QFile saved(root.path() + QLatin1Char('/') + files.first());
    ASSERT_TRUE(saved.open(QIODevice::ReadOnly));
    EXPECT_TRUE(saved.readAll().contains("\"type\":\"enhanced_codes\""));
    ASSERT_TRUE(disconnectAndWait(bridge));
}

TEST(BridgeTest, AClearReadsTheModulesEnhancedCodesAgain)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    SessionBridge bridge;
    bridge.setRecordingsRootForTesting(root.path());
    QStringList log;
    QObject::connect(&bridge, &SessionBridge::logMessage,
                     [&log](const QString& line) { log << line; });
    const auto lines = [&log]() {
        return log.filter(QStringLiteral("ECU 0x7E8 enhanced codes: "));
    };
    ASSERT_TRUE(bridge.connectSession(QStringLiteral("mock://demo")).isEmpty());
    bridge.readModuleCodes();
    ASSERT_TRUE(spinUntil([&]() { return lines().size() == 1; }, 4000));
    ASSERT_FALSE(bridge.clearDiagnostics().isEmpty());
    ASSERT_TRUE(spinUntil([&]() { return lines().size() == 2; }, 6000))
        << log.join('\n').toStdString();
    EXPECT_EQ(bridge.diagnostics()
                  .at(0)
                  .toMap()
                  .value("enhanced")
                  .toMap()
                  .value("outcome")
                  .toString(),
              QStringLiteral("answered"));
    ASSERT_TRUE(disconnectAndWait(bridge));
}
