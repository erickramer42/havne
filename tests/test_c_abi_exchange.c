/* File/layer: tests/test_c_abi_exchange.c | strict C part of test_c_abi
 * Testing: UNIT: the frames check, exports, imports, the CSV guess, the health report and obdb, compile and link, run as c_abi and c_abi_shared
 */

#include "ecuio/conditions.h"
#include "ecuio/queue.h"
#include "ecuio/obdb.h"
#include "ecuio/recording.h"
#include "ecuio/internal.h"
#include "ecuio/surrogate_protocol.h"
#include "ecuio/j2534_surrogate.h"
#include "ecuio/j2534.h"
#include "ecuio/stream.h"
#include "ecuio/serial.h"
#include "ecuio/plugin.h"
#include "ecuio/vin.h"
#include "ecuio/sim.h"
#include "ecuio/socketcan.h"
#include "ecuio/profile.h"
#include "ecuio/signal_sets.h"
#include "ecuio/signals.h"
#include "ecuio/core.h"
#include "ecuio/version.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "test_c_abi_parts.h"

/* A long call's cancel as C writes one (ecuio_cancel_t) */
static int never_cancel(void* user_data)
{
    (void)user_data;
    return 0;
}

int c_abi_exchange(void)
{
    ecuio_check_options_t check_options;
    ecuio_check_summary_t check_summary;
    ecuio_export_options_t export_options;
    ecuio_export_report_t export_report;
    ecuio_export_estimate_t estimate;
    uint32_t estimate_ms = 0;
    ecuio_import_options_t import_options;
    ecuio_import_report_t import_report;
    ecuio_csv_guess_t csv_guess;
    size_t csv_guess_needed = 1;
    ecuio_health_options_t health_options;
    ecuio_health_report_t health_report;
    ecuio_frames_read_t frames_read;

    /* The frames check (ADR-0049 section 6): its layout and refusals */
    memset(&check_options, 0, sizeof check_options);
    check_options.flags = 1;
    if (sizeof(ecuio_check_options_t) != 56 ||
        sizeof(ecuio_check_finding_t) != 56 ||
        offsetof(ecuio_check_finding_t, code) != 36 ||
        sizeof(ecuio_check_summary_t) != 128 ||
        offsetof(ecuio_check_summary_t, counts) != 24 ||
        offsetof(ecuio_check_summary_t, torn_rows) != 88 ||
        ECUIO_CHECK_GAP != 1 || ECUIO_CHECK_ASLEEP != 7 ||
        ecuio_recording_check(NULL, NULL, NULL, NULL, NULL, &check_summary) !=
            ECUIO_ERR_NULL_ARGUMENT ||
        ecuio_recording_check(".", NULL, NULL, NULL, NULL, NULL) !=
            ECUIO_ERR_NULL_ARGUMENT ||
        ecuio_recording_check(".", &check_options, NULL, NULL, NULL,
                              &check_summary) != ECUIO_ERR_INVALID_ARGUMENT) {
        return 1;
    }
    /* Exports and imports (ADR-0054): their layouts and refusals */
    memset(&export_options, 0, sizeof export_options);
    memset(&import_options, 0, sizeof import_options);
    export_options.format = ECUIO_EXPORT_PCAPNG;
    export_options.folder_utf8 = "."; /* the estimate judges the format */
    import_options.format = ECUIO_IMPORT_CANDUMP;
    if (sizeof(ecuio_export_report_t) != 120 ||
        offsetof(ecuio_export_report_t, failed_class) != 80 ||
        offsetof(ecuio_export_report_t, values) != 88 ||
        offsetof(ecuio_export_report_t, files) != 104 ||
        offsetof(ecuio_export_report_t, files_left_out) != 108 ||
        offsetof(ecuio_export_options_t, folder_utf8) != 24 ||
        offsetof(ecuio_export_options_t, layout) != 24 + 2 * sizeof(void*) ||
        offsetof(ecuio_export_options_t, columns_utf8) !=
            40 + 2 * sizeof(void*) ||
        sizeof(ecuio_export_options_t) != 56 + 2 * sizeof(void*) ||
        offsetof(ecuio_export_report_t, rows) != 112 ||
        sizeof(ecuio_export_estimate_t) != 40 || ECUIO_CSV_WIDE != 0u ||
        ECUIO_CSV_LONG != 1u || sizeof(ecuio_import_report_t) != 80 ||
        offsetof(ecuio_import_report_t, duration_us) != 40 ||
        offsetof(ecuio_import_report_t, values) != 48 ||
        offsetof(ecuio_import_report_t, columns_ignored) != 56 ||
        offsetof(ecuio_import_report_t, rows_no_time) != 64 ||
        offsetof(ecuio_import_report_t, reserved) != 72 ||
        offsetof(ecuio_import_options_t, in_path_utf8) != 8 ||
        ECUIO_EXPORT_LEFT_OUT_VIN_SHAPED != 4u ||
        ECUIO_EXPORT_LEFT_OUT_VIN_MASKED != 5u ||
        ECUIO_EXPORT_LEFT_OUT_COUNT != 6u ||
        ECUIO_FRAME_FLAG_FD_ESI != 0x200u ||
        ecuio_export(NULL, &export_report) != ECUIO_ERR_NULL_ARGUMENT ||
        ecuio_export(&export_options, NULL) != ECUIO_ERR_NULL_ARGUMENT ||
        ecuio_export_with(NULL, never_cancel, NULL, &export_report) !=
            ECUIO_ERR_NULL_ARGUMENT ||
        ecuio_export_estimate_with(NULL, &estimate_ms, 1, never_cancel, NULL,
                                   &estimate) != ECUIO_ERR_NULL_ARGUMENT ||
        ecuio_export_estimate(NULL, &estimate_ms, 1, &estimate) !=
            ECUIO_ERR_NULL_ARGUMENT ||
        ecuio_export_estimate(&export_options, &estimate_ms, 1, &estimate) !=
            ECUIO_ERR_INVALID_ARGUMENT ||
        ecuio_scrub_frames(0, NULL, NULL, NULL, 0, NULL, &export_report) !=
            ECUIO_ERR_NULL_ARGUMENT ||
        ecuio_import(NULL, &import_report) != ECUIO_ERR_NULL_ARGUMENT ||
        ecuio_import(&import_options, NULL) != ECUIO_ERR_NULL_ARGUMENT ||
        ecuio_import_with(NULL, never_cancel, NULL, &import_report) !=
            ECUIO_ERR_NULL_ARGUMENT) {
        return 1;
    }
    /* Frames read back (ADR-0049's appendix): the summary's layout, zeroed
     * by a refused call */
    memset(&frames_read, 0xFF, sizeof frames_read);
    if (sizeof(ecuio_frames_read_t) != 32 ||
        offsetof(ecuio_frames_read_t, torn_rows) != 8 ||
        offsetof(ecuio_frames_read_t, reserved0) != 12 ||
        offsetof(ecuio_frames_read_t, reserved) != 16 ||
        ecuio_recording_read_frames(NULL, 0, 0, NULL, NULL, &frames_read) !=
            ECUIO_ERR_NULL_ARGUMENT ||
        frames_read.rows != 0 || frames_read.torn_rows != 0 ||
        frames_read.reserved[1] != 0) {
        return 1;
    }
    /* The CSV map guess (ADR-0054's appendix): its layout and refusals */
    if (sizeof(ecuio_csv_guess_t) != 48 ||
        offsetof(ecuio_csv_guess_t, rows_read) != 12 ||
        offsetof(ecuio_csv_guess_t, map_offset) != 16 ||
        offsetof(ecuio_csv_guess_t, map_length) != 20 ||
        offsetof(ecuio_csv_guess_t, reserved) != 24 ||
        ECUIO_CSV_GUESS_TIME_UNIT_KNOWN != 0x1u ||
        ECUIO_CSV_GUESS_DECIMAL_COMMA != 0x2u ||
        ECUIO_CSV_GUESS_PREAMBLE != 0x4u ||
        ECUIO_CSV_GUESS_TIME_NUMBERS != 0x8u ||
        ecuio_import_csv_guess(NULL, NULL, NULL, 0, &csv_guess_needed,
                               &csv_guess) != ECUIO_ERR_NULL_ARGUMENT ||
        csv_guess_needed != 0 ||
        ecuio_import_csv_guess("log.csv", "h", NULL, 0, &csv_guess_needed,
                               NULL) != ECUIO_ERR_INVALID_ARGUMENT) {
        return 1;
    }
    /* The health report (ADR-0054's appendix): its layouts and refusals */
    memset(&health_options, 0, sizeof health_options);
    health_options.format = ECUIO_HEALTH_JSON;
    if (sizeof(ecuio_health_value_t) != 32 ||
        offsetof(ecuio_health_value_t, age_us) != 16 ||
        offsetof(ecuio_health_options_t, utc_us) != 32 ||
        offsetof(ecuio_health_options_t, baseline_folder_utf8) != 48 ||
        offsetof(ecuio_health_options_t, baseline_room) != 48 ||
        offsetof(ecuio_health_options_t, reserved) != 56 ||
        offsetof(ecuio_health_options_t, session) != 80 ||
        sizeof(ecuio_health_report_t) != 120 ||
        offsetof(ecuio_health_report_t, failed_class) != 80 ||
        ECUIO_HEALTH_TEXT != 2u || ECUIO_HEALTH_VIN_DECODED != 1u ||
        ECUIO_HEALTH_BASELINE_READ != 2u ||
        ecuio_health_export(NULL, &health_report) != ECUIO_ERR_NULL_ARGUMENT ||
        ecuio_health_export(&health_options, NULL) != ECUIO_ERR_NULL_ARGUMENT ||
        ecuio_health_export_with(NULL, never_cancel, NULL, &health_report) !=
            ECUIO_ERR_NULL_ARGUMENT) {
        return 1;
    }
    if (ecuio_obdb_import("{}", 2, NULL, NULL, NULL, NULL, NULL) !=
            ECUIO_ERR_NULL_ARGUMENT ||
        ECUIO_OBDB_NOTE_SKIPPED != 2u || ECUIO_OBDB_ALONE != 1u) {
        return 1;
    }
    ecuio_obdb_free(NULL);
    return 0;
}
