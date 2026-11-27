// File/layer: core/src/elm327_setup.cpp | C++17, Qt-free, every
//             platform (ADR-0026)
// Testing: HARNESS: Elm327SessionTest (test_elm327)
//
// The adapter's setup (ADR-0026 section 3), run at attach and after
// every reset or reopen, and the readings the session info takes from
// it (ADR-0019, ADR-0026 section 7).

#include "elm327_transport.h"

#include "ecuio/j2534_ptypes.h" /* ECUIO_PT_PROTOCOL_ISO15765; plain C */
#include "conversation_bind.h"
#include "error_detail.h"

#include <cstdio>
#include <cstring>

namespace ecuio_elm327 {

namespace {

/* A line for a detail: one line, cut to ELM327_QUOTE_MAX. */
void quote(const elm327_reader& r, char* out)
{
    size_t n = 0;
    for (; n < r.len && n < ELM327_QUOTE_MAX; ++n) {
        out[n] = r.line[n];
    }
    out[n] = '\0';
    ecuio_text_one_line(out);
}

} // namespace

/* A contract command (ADR-0026 section 3): its answer must be OK. An answer
 * that holds a reset's banner instead belonged to a reset still
 * finishing when the command went out (the MX+ on a new Bluetooth link
 * answered AT Z twice, the second time late:
 * docs/bench-sessions/2026-10-29-android.md), so the command is sent
 * once more; sending any contract command twice is harmless. */
setup_answer contract(transport* t, const char* cmd)
{
    setup_answer a;
    for (int attempt = 0; attempt < 2; ++attempt) {
        a = setup_answer{};
        bool reset = false;
        a.io = command(t, cmd, false, [&](const elm327_reader& r) {
            const elm327_line kind = elm327_classify(r, t->ext29, nullptr);
            if (kind == elm327_line::ok) {
                a.ok = true;
                return;
            }
            reset = reset || kind == elm327_line::reset;
            if (a.first[0] == '\0') {
                quote(r, a.first);
            }
        });
        a.ok = a.ok && a.io == elm327_io::prompt;
        if (a.ok || a.io != elm327_io::prompt || !reset) {
            break;
        }
        char sent[ELM327_CMD_MAX];
        bare_command(cmd, sent);
        elm327_log_write(t->log, "a reset's answer came for %s: sent again",
                         sent);
    }
    return a;
}

namespace {

/* A setup command's failure, as attach reports it: the port's
 * (LOAD_FAILED), silence or a wrong answer (VENDOR_REFUSED, naming the
 * command and what came back). The words go in the wire log too. */
ecuio_status_t refuse(transport* t, const char* cmd, const setup_answer& a,
                      const char* wanted)
{
    const ecuio_status_t st = (a.io == elm327_io::failed)
                                  ? ECUIO_ERR_LOAD_FAILED
                                  : ECUIO_ERR_VENDOR_REFUSED;
    char sent[ELM327_CMD_MAX];
    bare_command(cmd, sent);
    if (a.io == elm327_io::failed) {
        ecuio_detail_set("%s: the port failed during the adapter's setup",
                         t->port_name);
    } else if (a.io == elm327_io::silent) {
        ecuio_detail_set("the adapter on %s didn't answer %s within %s",
                         t->port_name, sent,
                         words_of(prompt_timeout_ms()).text);
    } else if (a.first[0] == '\0') {
        ecuio_detail_set("the adapter on %s answered %s with nothing, not %s",
                         t->port_name, sent, wanted);
    } else {
        ecuio_detail_set("the adapter on %s answered %s with '%s', not %s",
                         t->port_name, sent, a.first, wanted);
    }
    elm327_log_write(t->log, "the setup failed: %s", ecuio_last_error_detail());
    return st;
}

/* AT Z or AT I: ok when a line is an ELM327 version, copied to version
 * (when not NULL). With plain_ok, an OK line sets it instead of being
 * the answer quoted. */
setup_answer identify(transport* t, const char* cmd, char* version,
                      size_t version_cap, bool* plain_ok)
{
    setup_answer a;
    a.io = command(t, cmd, false, [&](const elm327_reader& r) {
        if (elm327_is_version(r.line, r.len)) {
            a.ok = true;
            if (version != nullptr) {
                copy_text(version, version_cap, r.line);
            }
        } else if (plain_ok != nullptr &&
                   elm327_classify(r, t->ext29, nullptr) == elm327_line::ok) {
            *plain_ok = true;
        } else if (a.first[0] == '\0') {
            quote(r, a.first);
        }
    });
    a.ok = a.ok && a.io == elm327_io::prompt;
    return a;
}

} // namespace

/* Whether the session asks nothing at all: it listens with AT MA
 * (ADR-0026's appendix "passive listening") */
bool asks_nothing(const transport* t)
{
    return t->engine.count == 0 && t->engine.scan_count == 0 &&
           !ecuio_session_may_talk(t->session, t->rx_ids, t->tx_ids,
                                   t->chan_count);
}

/* The setup's contract commands (ADR-0026 section 3), each answered before the
 * next: a known state, then everything the framing rests on. Any
 * answer but OK refuses, so an adapter never runs half-configured. The
 * reset is sent twice at most: an adapter still monitoring the bus takes
 * the first character as its stop and answers the rest '?'. A reset
 * answered OK alone (an OBDX) leaves the version to AT I, which must
 * give one. */
ecuio_status_t run_setup(transport* t, char* version, size_t version_cap)
{
    t->configured = false;
    t->header_set = false;
    t->reader = elm327_reader{};
    setup_answer reset;
    bool reset_ok = false;
    for (int attempt = 0; attempt < 2; ++attempt) {
        reset_ok = false;
        reset = identify(t, "ATZ\r", version, version_cap, &reset_ok);
        if (reset.io != elm327_io::prompt || reset.ok || reset_ok) {
            break;
        }
    }
    if (reset.io == elm327_io::prompt && !reset.ok && reset_ok) {
        const setup_answer id =
            identify(t, "ATI\r", version, version_cap, nullptr);
        if (!id.ok) {
            return refuse(t, "ATI\r", id, "an ELM327 version");
        }
    } else if (!reset.ok) {
        return refuse(t, "ATZ\r", reset, "an ELM327 version");
    }

    char sp[ELM327_CMD_MAX];
    snprintf(sp, sizeof(sp), "ATSP%c\r", t->protocol);
    /* A session that asks nothing only monitors, and needs no flow
     * control, so it is off. Under either setting the MX+'s STN2255
     * monitor prints a First Frame and none of its Consecutive Frames
     * (docs/bench-sessions/2026-11-04-two-ecu-car.md,
     * docs/bench-sessions/2026-11-05-two-ecu-car-rerun.md) */
    t->set_up_passive = asks_nothing(t);
    const char* const cfc = t->set_up_passive ? "ATCFC0\r" : "ATCFC1\r";
    const char* const steps[] = {
        "ATE0\r", /* no echo, no linefeeds, no spaces: fewer bytes */
        "ATL0\r",   "ATS0\r",
        "ATH1\r",   /* the CAN ID and the PCI byte on every line */
        "ATD0\r",   /* no data length digit after the ID (an STN's PP 29) */
        "ATCAF1\r", /* the adapter adds the PCI bytes when sending */
        cfc,        /* and the flow control after a First Frame, or none */
        sp,         /* ISO 15765-4 at the mode and rate, never a search */
        "ATAT1\r",  /* adaptive timing */
        "ATST19\r", /* the first reply's timeout, the engine's 100 ms */
    };
    for (const char* cmd : steps) {
        const setup_answer a = contract(t, cmd);
        if (!a.ok) {
            return refuse(t, cmd, a, "OK");
        }
    }
    char header_cmd[ELM327_CMD_MAX] = {};
    const setup_answer header = set_header(t, t->tx_ids[0], header_cmd);
    if (!header.ok) {
        return refuse(t, header_cmd, header, "OK");
    }
    const ecuio_status_t st = hearing_filter(t);
    if (st != ECUIO_OK) {
        return st;
    }
    t->configured = true;
    return ECUIO_OK;
}

/* The adapter's receive filter for these reply IDs (AT CRA for one, AT
 * CF and AT CM for several); a refusal as the setup's */
ecuio_status_t receive_filter(transport* t, const uint32_t* rx_ids,
                              uint32_t count)
{
    char filter[2][24];
    const size_t n =
        elm327_format_receive_filter(rx_ids, count, t->ext29, filter);
    for (size_t i = 0; i < n; ++i) {
        const setup_answer a = contract(t, filter[i]);
        if (!a.ok) {
            return refuse(t, filter[i], a, "OK");
        }
    }
    return ECUIO_OK;
}

/* The receive filter for the hearing modules' reply IDs (t->hearing);
 * nothing typed while none hears */
ecuio_status_t hearing_filter(transport* t)
{
    uint32_t ids[ECUIO_MAX_CHANNELS];
    uint32_t n = 0;
    for (uint32_t c = 0; c < t->chan_count && c < ECUIO_MAX_CHANNELS; ++c) {
        if (((t->hearing >> c) & 1u) != 0) {
            ids[n++] = t->rx_ids[c];
        }
    }
    return receive_filter(t, ids, n);
}

/* The start's emissions probe (request_probe.h): for each group, its
 * receive filter, the functional header and one 01 00, every line to the
 * prompt taken (the adapter waits its AT ST after the last reply). The
 * caller puts the channels' filter back; a refused command refuses the
 * connect, as the setup's do. */
ecuio_status_t run_probe(transport* t, ecuio_probe* p)
{
    const auto start = std::chrono::steady_clock::now();
    const uint32_t tx = t->ext29 ? ECUIO_PROBE_TX_29 : ECUIO_PROBE_TX_11;
    uint32_t group[ECUIO_MAX_CHANNELS];
    for (uint32_t g = 0;; ++g) {
        const uint32_t n = ecuio_probe_group(t->rx_ids, t->chan_count, t->ext29,
                                             t->engine.module_budget, g, group);
        if (n == 0) {
            break;
        }
        ecuio_status_t st = receive_filter(t, group, n);
        if (st != ECUIO_OK) {
            return st;
        }
        char header_cmd[ELM327_CMD_MAX] = {};
        const setup_answer h = set_header(t, tx, header_cmd);
        if (!h.ok) {
            return refuse(t, header_cmd, h, "OK");
        }
        char cmd[ELM327_CMD_MAX];
        (void)elm327_format_request(ECUIO_PROBE_REQUEST, 2, 0, cmd, sizeof cmd);
        ++p->requests;
        const elm327_io io =
            command(t, cmd, false, [&](const elm327_reader& r) {
                elm327_can_line f;
                if (elm327_classify(r, t->ext29, &f) != elm327_line::frame) {
                    return;
                }
                isotp_message m;
                if (isotp_take(t->isotp, f, 0, &m) == isotp_result::complete) {
                    ecuio_probe_take(*p, m.id, m.data, m.len);
                }
            });
        if (io == elm327_io::failed) {
            setup_answer a;
            a.io = io;
            return refuse(t, cmd, a, "a reply");
        }
    }
    p->took_us = std::chrono::duration_cast<std::chrono::microseconds>(
                     std::chrono::steady_clock::now() - start)
                     .count();
    return ECUIO_OK;
}

namespace {

/* A reading's first line that matches (ADR-0026 section 3): anything else,
 * '?' included, means "not present", and nothing fails. */
template <typename Match>
bool reading(transport* t, const char* cmd, char* out, size_t cap,
             Match&& match)
{
    bool found = false;
    const elm327_io io = command(t, cmd, false, [&](const elm327_reader& r) {
        if (!found && match(r)) {
            copy_text(out, cap, r.line);
            found = true;
        }
    });
    return io == elm327_io::prompt && found;
}

} // namespace

bool read_battery(transport* t, uint32_t* mv)
{
    char volts[ELM327_LINE_MAX + 1] = {};
    return reading(t, t->stn ? "STVR\r" : "ATRV\r", volts, sizeof(volts),
                   [&](const elm327_reader& r) {
                       return elm327_parse_millivolts(r.line, r.len, mv);
                   });
}

/* The session info (ADR-0019, ADR-0026 section 7): what the session runs on,
 * then the readings. A reading the adapter refuses leaves its flag
 * clear. */
void describe_session(transport* t, const char* reset_version,
                      ecuio_session_info_t* info)
{
    *info = ecuio_session_info_t{};
    info->backend = ECUIO_BACKEND_ELM327;
    info->protocol_id = static_cast<uint32_t>(ECUIO_PT_PROTOCOL_ISO15765);
    info->baud = static_cast<uint32_t>(t->can_baud);

    char version[ELM327_LINE_MAX + 1] = {};
    if (!reading(t, "ATI\r", version, sizeof(version),
                 [](const elm327_reader& r) {
                     return elm327_is_version(r.line, r.len);
                 })) {
        copy_text(version, sizeof(version), reset_version);
    }
    char stn[ELM327_LINE_MAX + 1] = {};
    const bool is_stn =
        reading(t, "STI\r", stn, sizeof(stn), [](const elm327_reader& r) {
            return elm327_is_stn(r.line, r.len);
        });
    char* fw = info->firmware_version;
    copy_text(fw, sizeof(info->firmware_version), version);
    if (is_stn) {
        const size_t used = strlen(fw);
        copy_text(fw + used, sizeof(info->firmware_version) - used, ", ");
        copy_text(fw + strlen(fw), sizeof(info->firmware_version) - strlen(fw),
                  stn);
    }
    ecuio_text_one_line(info->firmware_version);
    if (info->firmware_version[0] != '\0') {
        info->flags |= ECUIO_SESSION_INFO_VERSIONS;
    }

    t->stn = is_stn;
    uint32_t mv = 0;
    if (read_battery(t, &mv)) {
        info->vbatt_mv = mv;
        info->flags |= ECUIO_SESSION_INFO_VBATT;
    }

    if (is_stn) {
        /* Parameter 2A on with bit 2 clear: no 7F xx 78 processing */
        char line[ELM327_LINE_MAX + 1] = {};
        uint8_t value = 0;
        bool on = false;
        if (reading(t, "ATPPS\r", line, sizeof(line),
                    [&](const elm327_reader& r) {
                        return elm327_pps_value(r.line, r.len, 0x2A, &value,
                                                &on);
                    }) &&
            on && (value & 0x04) == 0) {
            info->flags |= ECUIO_SESSION_INFO_PENDING_OFF;
        }
    }
}

} // namespace ecuio_elm327
