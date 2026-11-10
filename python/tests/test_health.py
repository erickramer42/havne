# File/layer: python/tests/test_health.py | pytest
# Testing: HARNESS, PythonTest.test_health: havne.health_report and
#          Session.health_report against havne-cli health's own file
#
# A recording's report written through the package and through havne-cli
# health (HAVNE_CLI) over the same folder, with the package's time and model
# year the ones the CLI's file shows: the same bytes in both formats, the
# counts of the CLI's health line, a baseline read, unread and none, the
# same bytes twice for one time. A connected session's report (a replay of
# the fixture under obd2, never a bus): no key values until a stream has
# read some, the newest of each after, aged from its read, replaced by
# values=, settled as reports_settled() says, no range and no cancel. The
# arguments refused before core is called, a failed check as CheckFailed,
# and the cancel of a recording's report as the exports'.

import datetime
import json
import os
import signal
import threading

import pytest

import havne
from conftest import FINDINGS, Asks, cli, line_of, nothing_made, partials, replay_uri
from havne import health

NO_SUCH = "no-such-baseline"


def utc_us_of(path):
    """The report's time as the file shows it, in whole seconds"""
    with open(path, encoding="utf-8") as f:
        body = f.read()
    if body.startswith("{"):
        word = json.loads(body)["utc"]
    else:
        word = next(x for x in body.splitlines() if x.startswith("Time: "))[len("Time: "):]
    when = datetime.datetime.strptime(word, "%Y-%m-%dT%H:%M:%SZ").replace(
        tzinfo=datetime.timezone.utc)
    return int(when.timestamp()) * 1_000_000


def this_year_plus_one():
    return datetime.datetime.now(datetime.timezone.utc).year + 1


def cli_report(tmp_path, folder, fmt, *args):
    out = tmp_path / f"cli.{fmt}"
    line = line_of(cli("health", folder, out, "--format", fmt, *args), "health")
    return out, line


def python_like(tmp_path, folder, fmt, cli_out, **kw):
    return havne.health_report(folder, tmp_path / f"py.{fmt}", fmt,
                               utc_us=utc_us_of(cli_out), **kw)


def same_counts(got, line):
    assert (got.format, got.source) == (line["format"], line["source"])
    assert (got.ecus, got.codes, got.values, got.since_cleared) == \
        (line["ecus"], line["codes"], line["values"], line["since_cleared"])
    assert got.left_out.identification == line["left_out"]["identification"]
    assert got.left_out[1:] == (0, 0, 0)
    assert got.vin_decoded == line["vin_decoded"]
    assert {None: "none", True: "read", False: "unread"}[got.baseline_read] == line["baseline"]
    assert got.settled is None


@pytest.mark.parametrize("fmt", ["json", "text"])
def test_a_recordings_report_is_the_clis_in_both_formats(tmp_path, health_car, fmt):
    out, line = cli_report(tmp_path, health_car, fmt)
    got = python_like(tmp_path, health_car, fmt, out)
    assert (tmp_path / f"py.{fmt}").read_bytes() == out.read_bytes()
    same_counts(got, line)
    assert isinstance(got, havne.HealthReport) and isinstance(got.left_out, havne.LeftOut)
    assert got.path == str(tmp_path / f"py.{fmt}") and got.settled is None
    assert got.ecus == 1 and got.codes == 1 and got.vin_decoded and got.values >= 1
    assert got.left_out.identification >= 1


def test_the_vin_and_the_folder_are_in_neither_the_file_nor_the_result(tmp_path, health_car):
    got = havne.health_report(health_car, tmp_path / "r.json", "json")
    body = (tmp_path / "r.json").read_text(encoding="utf-8")
    for leak in ("ZZZFAKE", health_car, os.path.basename(health_car), "Daily driver"):
        assert leak not in body and leak not in repr(got)


def test_units_and_a_range_are_the_clis(tmp_path, health_car):
    args = ["--units", "imperial", "--from", "0.15", "--to", "0.5"]
    out, line = cli_report(tmp_path, health_car, "json", *args)
    got = python_like(tmp_path, health_car, "json", out, units="imperial", from_us=150_000,
                      to_us=500_000)
    assert (tmp_path / "py.json").read_bytes() == out.read_bytes()
    same_counts(got, line)
    whole = havne.health_report(health_car, tmp_path / "whole.json", "json")
    assert (tmp_path / "whole.json").read_bytes() != out.read_bytes()
    assert whole.values >= got.values


def test_a_baseline_read_unread_and_none_are_the_clis(tmp_path, health_car):
    findings = FINDINGS
    for name, given, want in [("read", findings, True), ("unread", tmp_path / NO_SUCH, False),
                              ("none", None, None)]:
        sub = tmp_path / name
        sub.mkdir()
        args = [] if given is None else ["--baseline", given]
        out, line = cli_report(sub, health_car, "json", *args)
        got = python_like(sub, health_car, "json", out, baseline=given)
        assert (sub / "py.json").read_bytes() == out.read_bytes()
        same_counts(got, line)
        assert got.baseline_read is want
        body = (sub / "py.json").read_text(encoding="utf-8")
        assert (NO_SUCH in body) is False and findings not in body


def test_one_time_gives_the_same_bytes_twice_and_another_time_another(tmp_path, health_car):
    for i, at in enumerate((1_700_000_000_000_000, 1_700_000_000_000_000, 1_700_000_100_000_000)):
        havne.health_report(health_car, tmp_path / f"{i}.json", "json", utc_us=at,
                            latest_model_year=2027)
    one, two, three = ((tmp_path / f"{i}.json").read_bytes() for i in range(3))
    assert one == two and one != three
    havne.health_report(health_car, tmp_path / "none.json", "json", utc_us=0)
    assert json.loads((tmp_path / "none.json").read_text(encoding="utf-8"))["utc"] is None


def test_the_defaults_are_the_clock_and_this_year_plus_one(tmp_path, health_car, monkeypatch):
    seen = {}
    real = health.lib.ecuio_health_export_with

    def spy(options, callback, user, report):
        o = options._obj
        seen.update(utc=o.utc_us, year=o.latest_model_year)
        return real(options, callback, user, report)

    monkeypatch.setattr(health.lib, "ecuio_health_export_with", spy)
    before = health._clock_us()
    havne.health_report(health_car, tmp_path / "now.json", "json")
    assert before <= seen["utc"] <= health._clock_us() and seen["year"] == this_year_plus_one()
    havne.health_report(health_car, tmp_path / "set.json", "json", utc_us=5, latest_model_year=0)
    assert (seen["utc"], seen["year"]) == (5, 0)


# --- arguments ----------------------------------------------------------

def test_a_wrong_argument_is_refused_before_core_is_called(tmp_path, health_car, monkeypatch):
    calls = []
    monkeypatch.setattr(health.lib, "ecuio_health_export_with", lambda *a: calls.append(a))
    out = tmp_path / "x"
    v, t = ValueError, TypeError
    for kind, kw in [(v, dict(format="xml")), (t, dict(format=None)), (v, dict(format="JSON")),
                     (v, dict(units="si")), (v, dict(to_us=0)), (v, dict(from_us=3, to_us=2)),
                     (t, dict(from_us=1.5)), (t, dict(to_us=True)), (t, dict(utc_us="now")),
                     (t, dict(utc_us=1.5)), (v, dict(utc_us=-1)), (t, dict(latest_model_year=2.5)),
                     (v, dict(latest_model_year=-1)), (v, dict(latest_model_year=2 ** 32)),
                     (v, dict(baseline="")), (t, dict(baseline=3)), (t, dict(cancel=1))]:
        kw = {"format": "json", **kw}
        with pytest.raises(kind):
            havne.health_report(health_car, out, kw.pop("format"), **kw)
    with pytest.raises(TypeError):
        havne.health_report(health_car, out)  # the format is not defaulted
    assert calls == [] and nothing_made(tmp_path, "x")


def test_core_refuses_what_it_refuses_and_leaves_what_was_there(tmp_path, health_car):
    out = tmp_path / "taken.json"
    out.write_text("mine", encoding="utf-8")
    with pytest.raises(havne.Error) as e:
        havne.health_report(health_car, out, "json")
    assert e.value.status == havne.c.ECUIO_ERR_LOAD_FAILED and "exists" in e.value.detail
    assert e.value.call == "ecuio_health_export_with" and not isinstance(e.value, havne.CheckFailed)
    assert out.read_text(encoding="utf-8") == "mine" and not partials(tmp_path, "taken.json")
    with pytest.raises(havne.Error) as e:
        havne.health_report(tmp_path / "nowhere", tmp_path / "n.json", "json")
    assert e.value.status == havne.c.ECUIO_ERR_LOAD_FAILED
    assert nothing_made(tmp_path, "n.json")
    with pytest.raises(havne.Error):
        havne.health_report(health_car, tmp_path / "late.json", "json", from_us=10 ** 12)
    assert nothing_made(tmp_path, "late.json")


@pytest.mark.parametrize("failed, word", [(1, "identification"), (2, "unattributable"),
                                          (3, "text_values"), (4, "activity"),
                                          (5, "vin_shaped"), (9, "unknown")])
def test_a_failed_check_is_check_failed_with_its_class(tmp_path, health_car, monkeypatch,
                                                       failed, word):
    # Core's check finds a class only in a file that holds a value of it,
    # which no honest recording makes: the call's answer is stood in for
    real = health.lib

    class Fails:
        def __getattr__(self, name):
            return getattr(real, name)

        def ecuio_health_export_with(self, options, callback, user, report):
            report._obj.failed_class = failed
            return havne.c.ECUIO_ERR_LOAD_FAILED

    monkeypatch.setattr(health, "lib", Fails())
    with pytest.raises(havne.CheckFailed) as e:
        havne.health_report(health_car, tmp_path / "f.json", "json")
    assert e.value.failed_class == word and isinstance(e.value, havne.Error)
    assert e.value.call == "ecuio_health_export_with"
    # A refusal with no class stays a plain Error
    Fails.ecuio_health_export_with = lambda self, o, c, u, r: havne.c.ECUIO_ERR_LOAD_FAILED
    with pytest.raises(havne.Error) as e:
        havne.health_report(health_car, tmp_path / "f.json", "json")
    assert not isinstance(e.value, havne.CheckFailed)


# --- the cancel ---------------------------------------------------------

def run_report(tmp_path, name, cancel, folder):
    return havne.health_report(folder, tmp_path / name, "json", utc_us=1, cancel=cancel)


@pytest.fixture
def run(health_car):
    return lambda tmp_path, name, cancel: run_report(tmp_path, name, cancel, health_car)


def test_a_cancel_that_never_stops_gives_the_same_report(tmp_path, run):
    asks = Asks()
    with_cancel = run(tmp_path, "with.json", asks)
    without = run(tmp_path, "without.json", None)
    assert asks.asked >= 2
    assert with_cancel._replace(path="") == without._replace(path="")
    assert (tmp_path / "with.json").read_bytes() == (tmp_path / "without.json").read_bytes()


def test_a_stop_at_each_ask_in_turn_leaves_nothing(tmp_path, run):
    whole = Asks()
    run(tmp_path, "whole.json", whole)
    for at in range(1, whole.asked + 1):
        asks = Asks(at)
        with pytest.raises(havne.Error) as e:
            run(tmp_path, f"stop-{at}.json", asks)
        assert e.value.status == havne.c.ECUIO_ERR_CANCELED
        assert e.value.detail == "the report was canceled" or "canceled" in e.value.words
        assert asks.asked == at and nothing_made(tmp_path, f"stop-{at}.json")


def test_an_exception_and_a_ctrl_c_reach_the_caller(tmp_path, run):
    asks = Asks(1, RuntimeError("the caller's own"))
    with pytest.raises(RuntimeError, match="the caller's own"):
        run(tmp_path, "raised.json", asks)
    assert nothing_made(tmp_path, "raised.json")
    asks = Asks(2, KeyboardInterrupt())
    with pytest.raises(KeyboardInterrupt):
        run(tmp_path, "interrupted.json", asks)
    assert nothing_made(tmp_path, "interrupted.json")
    during = []

    def ctrl_c():
        during.append(signal.getsignal(signal.SIGINT))
        signal.raise_signal(signal.SIGINT)
        return False

    with pytest.raises(KeyboardInterrupt):
        run(tmp_path, "ctrl-c.json", Asks(1, ctrl_c))
    assert nothing_made(tmp_path, "ctrl-c.json")
    assert during[0] is not signal.default_int_handler
    assert signal.getsignal(signal.SIGINT) is signal.default_int_handler


def test_a_report_off_the_main_thread_leaves_the_handler_alone(tmp_path, run):
    got, asks = [], Asks()
    thread = threading.Thread(target=lambda: got.append(run(tmp_path, "thread.json", asks)))
    thread.start()
    thread.join()
    assert len(got) == 1 and asks.asked >= 2
    assert signal.getsignal(signal.SIGINT) is signal.default_int_handler


# --- a connected session ------------------------------------------------

def replayed(**kw):
    return havne.Session(replay_uri("?profile=obd2&speed=max"), **kw)


def drained(stream):
    for _ in stream:
        pass


def test_a_sessions_report_has_no_key_values_until_a_stream_has_read_some(tmp_path):
    with replayed() as s:
        values = s.values(capacity=65536)
        s.start()
        # Nothing the script has read: no key values, and the report says so
        none = s.health_report(tmp_path / "none.json", "json", utc_us=1)
        assert none.values == 0 and none.source == "session" and none.settled is True
        assert none.baseline_read is None
        drained(values)
        some = s.health_report(tmp_path / "some.json", "json", utc_us=1)
        assert some.values > 0
        body = json.loads((tmp_path / "some.json").read_text(encoding="utf-8"))
        assert len(body["key_values"]) == some.values
        assert {k["name"] for k in body["key_values"]} <= {n for n, _ in s.signals.values()}
        text = s.health_report(tmp_path / "some.txt", "text", utc_us=1)
        assert text.values == some.values
    with replayed() as s:
        s.start()   # no values stream at all
        assert s.health_report(tmp_path / "nostream.json", "json", utc_us=1).values == 0


def test_a_sessions_report_holds_the_newest_value_of_each_signal(tmp_path):
    with replayed() as s:
        values = s.values(capacity=65536)
        s.start()
        seen = {}
        for v in values:
            if v.kind not in (havne.c.ECUIO_VALUE_TEXT, havne.c.ECUIO_VALUE_HEX):
                seen[v.name] = v.value
        s.health_report(tmp_path / "n.json", "json", utc_us=1)
        shown = {k["name"]: k["value"] for k in json.loads(
            (tmp_path / "n.json").read_text(encoding="utf-8"))["key_values"]}
        # The report rounds to the signal's decimals: the last value read, to half a unit
        assert shown and all(abs(shown[n] - seen[n]) <= 0.5 for n in shown)


def test_the_values_are_aged_from_their_read(tmp_path, monkeypatch):
    clock = [10_000_000_000]
    monkeypatch.setattr(havne.session.time, "monotonic_ns", lambda: clock[0])
    with replayed() as s:
        values = s.values(capacity=65536)
        s.start()
        drained(values)
        clock[0] += 2_000_000_000
        s.health_report(tmp_path / "fresh.json", "json", utc_us=1)
        clock[0] += 9_000_000_000
        s.health_report(tmp_path / "stale.json", "json", utc_us=1)
    fresh = json.loads((tmp_path / "fresh.json").read_text(encoding="utf-8"))["key_values"]
    stale = json.loads((tmp_path / "stale.json").read_text(encoding="utf-8"))["key_values"]
    assert fresh and all(k["age_ms"] == 2000 and not k["stale"] for k in fresh)
    assert all(k["age_ms"] == 11000 and k["stale"] for k in stale)


def test_two_streams_give_the_newest_read_of_each_signal(tmp_path, monkeypatch):
    clock = [10_000_000_000]
    monkeypatch.setattr(havne.session.time, "monotonic_ns", lambda: clock[0])
    with replayed() as s:
        first, second = s.values(capacity=65536), s.values(capacity=65536)
        s.start()
        drained(first)
        clock[0] += 1_000_000_000
        drained(second)
        clock[0] += 500_000_000
        s.health_report(tmp_path / "two.json", "json", utc_us=1)
    ages = {k["age_ms"] for k in json.loads(
        (tmp_path / "two.json").read_text(encoding="utf-8"))["key_values"]}
    assert ages == {500}, "aged from the second stream's later read"


def test_a_signal_read_again_is_aged_from_its_newest_read(tmp_path, monkeypatch):
    clock = [0]
    monkeypatch.setattr(havne.session.time, "monotonic_ns", lambda: clock[0])
    with havne.Session("mock://py?profile=mock") as s:
        values = s.values(capacity=65536)
        s.start()
        clock[0] = 1_000_000_000
        first = []
        while not first:
            first = values.read(0.1)
        stream = s._streams[0]
        at = stream._newest()
        assert at and {t for _, t in at.values()} == {1_000_000_000}
        clock[0] = 5_000_000_000
        later = []
        while not later:
            later = values.read(0.1)
        newest = stream._newest()
        for v in later:
            assert newest[v.signal_id][1] == 5_000_000_000


def test_text_and_hex_values_are_never_held():
    with havne.Session("mock://py?profile=uds-generic&did=7E8:F190") as s:
        values = s.values()
        s.start()
        got = []
        while not got:
            got = values.read(0.2)
        assert any(v.kind in (havne.c.ECUIO_VALUE_TEXT, havne.c.ECUIO_VALUE_HEX) for v in got)
        held = s._streams[0]._newest()
        assert held == {}, "a stream of text alone holds nothing"


def test_values_replace_the_held_ones(tmp_path):
    with replayed() as s:
        values = s.values(capacity=65536)
        s.start()
        drained(values)
        names = [n for n, _ in s.signals.values()]
        held = s.health_report(tmp_path / "held.json", "json", utc_us=1)
        mine = s.health_report(tmp_path / "mine.json", "json", utc_us=1,
                               values=[("RPM", 3000.0, 0)])
        assert mine.values == 1 and held.values > 1 and "RPM" in names
        body = json.loads((tmp_path / "mine.json").read_text(encoding="utf-8"))
        assert [(k["name"], k["value"], k["age_ms"]) for k in body["key_values"]] == \
            [("RPM", 3000, 0)]
        by_id = s.health_report(tmp_path / "id.json", "json", utc_us=1,
                                values=[(next(i for i, (n, _) in s.signals.items()
                                              if n == "RPM"), 3000, 0)])
        assert by_id.values == 1
        assert (tmp_path / "id.json").read_bytes() == (tmp_path / "mine.json").read_bytes()
        empty = s.health_report(tmp_path / "empty.json", "json", utc_us=1, values=[])
        assert empty.values == 0, "an empty values is none, not the held ones"
        for bad, kind in [([("RPM", 1.0)], TypeError), ([("RPM", "1", 0)], TypeError),
                          ([("RPM", True, 0)], TypeError), ([("RPM", 1.0, -1)], ValueError),
                          ([("RPM", 1.0, 1.5)], TypeError), ([(True, 1.0, 0)], ValueError),
                          ([(70000, 1.0, 0)], ValueError), ([("RPM", 1, 0), ("RPM", 2, 0)],
                                                          ValueError),
                          ([("No Such", 1.0, 0)], KeyError), (["RPM"], TypeError)]:
            with pytest.raises(kind):
                s.health_report(tmp_path / "bad.json", "json", values=bad)
        assert nothing_made(tmp_path, "bad.json")


def test_a_sessions_report_has_no_range_and_no_cancel(tmp_path):
    with replayed() as s:
        for kw in (dict(from_us=0), dict(to_us=5), dict(cancel=lambda: False)):
            with pytest.raises(TypeError):
                s.health_report(tmp_path / "x.json", "json", **kw)
        with pytest.raises(TypeError):
            s.health_report(tmp_path / "x.json")
        with pytest.raises(ValueError):
            s.health_report(tmp_path / "x.json", "xml")
        assert nothing_made(tmp_path, "x.json")


def test_a_mock_sessions_report_names_neither_its_label_nor_a_path(tmp_path):
    with havne.Session("mock://Weekend car?profile=obd2") as s:
        got = s.health_report(tmp_path / "m.json", "json")
        body = (tmp_path / "m.json").read_text(encoding="utf-8")
        assert "Weekend car" not in body and "Weekend car" not in repr(got)
        assert "mock://<label>?profile=obd2" in body
        assert got.ecus == 1 and got.settled is True and got.values == 0
        with pytest.raises(havne.Error) as e:
            s.health_report(tmp_path / "m.json", "json")
        assert e.value.call == "ecuio_health_export"
    with pytest.raises(ValueError):
        s.health_report(tmp_path / "closed.json", "json")


def test_the_report_settles_when_its_diagnostics_are_in(monkeypatch):
    with havne.Session("mock://py?profile=obd2") as s:
        assert s.reports_settled() is True
        real = s.diagnostics

        def asking(channel=0):
            d = real(channel)
            d.flags = (d.flags | havne.c.ECUIO_DIAG_ASKED) & ~havne.c.ECUIO_DIAG_COMPLETE
            return d

        monkeypatch.setattr(s, "diagnostics", asking)
        assert s.reports_settled() is False
        monkeypatch.setattr(s, "diagnostics", real)
        ident = s.identity

        def unfinished(channel=0):
            i = ident(channel)
            i.flags &= ~havne.c.ECUIO_IDENTITY_COMPLETE
            i.items = 3
            return i

        monkeypatch.setattr(s, "identity", unfinished)
        assert s.reports_settled() is False
    with replayed() as s:
        assert s.info.channel_count == 0 and s.reports_settled() is True
