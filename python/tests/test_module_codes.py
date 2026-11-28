# File/layer: python/tests/test_module_codes.py | pytest
# Testing: HARNESS, PythonTest.test_module_codes: a module's own codes through
#          UDS 0x19 on mock://, the loop, and its lines against havne-cli
#          enhanced-codes (HAVNE_CLI), field for field

import pytest

import havne
from havne import c
from conftest import cli, line_of

CODES = ["P0128-00", "U1123-45", "B1A01-1C", "P0301-00"]


def written(report, n):
    return [havne.dtc_format_uds(report.codes[i].code, report.format) for i in range(n)]


def test_the_mocks_codes_read_under_the_default_mask():
    with havne.Session("mock://py") as s:
        s.start()
        report = s.wait_module_codes(s.read_module_codes(0))
    assert report is not None and report.mask == 0xAF
    assert report.count_outcome == c.ECUIO_DIAG_ANSWERED
    assert report.list_outcome == c.ECUIO_DIAG_ANSWERED
    assert written(report, report.held) == CODES


def test_a_masks_bits_choose_the_codes():
    with havne.Session("mock://py") as s:
        s.start()
        report = s.wait_module_codes(s.read_module_codes(0, 0xFF))
    assert report.mask == 0xFF and report.held == 5


def test_a_wait_that_runs_out_gives_none():
    with havne.Session("mock://py") as s:
        s.start()
        assert s.wait_module_codes(10 ** 6, timeout_s=0.2) is None


def test_a_channel_past_the_modules_is_refused():
    with havne.Session("mock://py") as s:
        s.start()
        with pytest.raises(havne.Error) as e:
            s.read_module_codes(5)
        assert e.value.name == "INVALID_ARGUMENT"


def test_the_loop_reads_every_module_in_order():
    with havne.Session("mock://py") as s:
        s.start()
        reports, not_reached = s.read_all_module_codes()
    assert not_reached == 0 and [r.rx_id for r in reports] == [0x7E8]
    assert written(reports[0], reports[0].held) == CODES


def test_the_words_come_from_core():
    assert havne.dtc_format_uds(0x012800) == "P0128-00"
    assert havne.dtc_format_uds(0x012800, c.ECUIO_DTC_FORMAT_J1939) == "0x012800"
    assert havne.dtc_title_uds(0x030100) == "Cylinder 1 Misfire Detected"
    assert havne.dtc_title_uds(0x012800, c.ECUIO_DTC_FORMAT_J1939) is None
    assert [havne.dtc_status_state(x) for x in (0x01, 0x08)] == ["Active", "Not now"]
    assert [havne.dtc_status_class(x) for x in (0x08, 0x04, 0x20, 0x01)] == \
        ["Stored", "Pending", "History", None]


def test_a_code_in_the_standard_list_is_marked():
    with havne.Session("mock://py") as s:
        s.start()
        report = s.wait_module_codes(s.read_module_codes(0))
        standard = s.diagnostics(0)
    assert havne.module_code_emissions_lists(standard, 0x030100) == ["stored"]
    assert havne.module_code_emissions_lists(standard, 0x012800) == []
    assert havne.module_code_emissions_lists(standard, 0x030100,
                                             c.ECUIO_DTC_FORMAT_J1939) == []
    assert report.held == 4


def test_the_clis_line_says_what_python_reads():
    lines = cli("enhanced-codes", "mock://py")
    line = line_of(lines, "enhanced_codes")
    with havne.Session("mock://py") as s:
        s.start()
        reports, _ = s.read_all_module_codes()
        standard = s.diagnostics(0)
    report = reports[0]
    n = report.held
    assert line["list"]["codes"] == written(report, n)
    assert line["list"]["state"] == [havne.dtc_status_state(report.codes[i].status)
                                     for i in range(n)]
    assert line["list"]["class"] == [havne.dtc_status_class(report.codes[i].status)
                                     for i in range(n)]
    assert line["list"]["emissions"] == [
        havne.module_code_emissions_lists(standard, report.codes[i].code) for i in range(n)]
    assert line_of(lines, "enhanced_codes_end")["read"] == len(reports)


def stubbed(flags_list, read_id=1):
    reports = []
    for flags in flags_list:
        r = c.ecuio_module_codes_t()
        r.flags = flags
        r.read_id = read_id
        reports.append(r)
    return iter(reports)


def test_a_report_still_reading_is_not_the_end(monkeypatch):
    with havne.Session("mock://py") as s:
        seen = stubbed([c.ECUIO_MODULE_CODES_READING, c.ECUIO_MODULE_CODES_COMPLETE])
        monkeypatch.setattr(s, "module_codes", lambda channel=0: next(seen))
        report = s.wait_module_codes(1, timeout_s=5)
        assert report.flags & c.ECUIO_MODULE_CODES_COMPLETE
        assert next(seen, None) is None


def test_an_earlier_reads_report_is_not_the_one_waited_for(monkeypatch):
    with havne.Session("mock://py") as s:
        old = c.ecuio_module_codes_t()
        old.flags = c.ECUIO_MODULE_CODES_COMPLETE
        old.read_id = 1
        monkeypatch.setattr(s, "module_codes", lambda channel=0: old)
        assert s.wait_module_codes(2, timeout_s=0.1) is None


def test_a_module_that_could_not_be_opened_is_not_reached(monkeypatch):
    with havne.Session("mock://py") as s:
        s.start()
        closed = c.ecuio_module_codes_t()
        closed.flags = c.ECUIO_MODULE_CODES_COMPLETE | c.ECUIO_MODULE_CODES_NOT_OPENED
        closed.read_id = 1
        monkeypatch.setattr(s, "module_codes", lambda channel=0: closed)
        reports, not_reached = s.read_all_module_codes()
    assert reports == [] and not_reached == 1
