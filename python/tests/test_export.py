# File/layer: python/tests/test_export.py | pytest
# Testing: HARNESS, PythonTest.test_export: havne.export_pcapng, export_csv,
#          export_folder and estimate_csv against havne-cli's own files
#
# Each format, each option and the sizing written through the package and
# through havne-cli export (HAVNE_CLI) over the same recording: the same
# bytes, the same counts. A recording of the replay fixture under obd2
# (frames and values), the findings fixture (a torn ISO-TP message the
# scrub leaves out), a values-only import with a text signal and a
# frames-only one that echoes its VIN. A named column the recording lacks,
# the sizing's bytes as the export writes them, the arguments refused before
# core is called, core's refusals and a failed check as CheckFailed. The
# cancel stopping each call at each of its asks in turn, raising, or a
# Ctrl+C: nothing written, the exception reaching the caller.

import os
import signal
import threading

import pytest

import havne
from conftest import FINDINGS, Asks, cli, line_of, nothing_made, partials

CLASSES = ("identification", "unattributable", "text_values", "activity")


def tree(root):
    """{relative path: bytes} of a folder, or of one file"""
    if os.path.isfile(root):
        with open(root, "rb") as f:
            return {"": f.read()}
    got = {}
    for here, _, names in os.walk(root):
        for n in names:
            path = os.path.join(here, n)
            with open(path, "rb") as f:
                got[os.path.relpath(path, root).replace("\\", "/")] = f.read()
    return got


def seconds(us):
    return f"{us / 1e6:.6f}"


def middle(folder):
    """(from_us, to_us): a whole-ms stretch inside the recording's values"""
    with havne.Recording(folder) as r:
        first = min(s.first_us for s in r.signals if s.first_us is not None)
        last = max(s.last_us for s in r.signals if s.last_us is not None)
    lo = (first + (last - first) // 4) // 1000 * 1000
    hi = (first + 3 * (last - first) // 4) // 1000 * 1000
    assert hi > lo
    return lo, hi


def picks(folder):
    """Two signals the recording has, in the reverse of its order"""
    with havne.Recording(folder) as r:
        names = [s.name for s in r.signals if s.kind == "number" and s.samples]
    assert len(names) >= 2
    return [names[1], names[0]]


def same_counts(got, line):
    assert got.format == line["format"] and got.scrubbed == line["scrubbed"]
    assert (got.frames, got.made_up) == (line["frames"], line["made_up"])
    assert dict(zip(CLASSES, got.left_out)) == line["left_out"]
    if "values" in line:
        assert (got.values, got.signal_count) == (line["values"], line["signals"])
    if "rows" in line:
        assert (got.rows, len(got.missing)) == (line["rows"], line["missing"])
    if "files" in line:
        assert (got.files, got.files_left_out) == (line["files"], line["files_left_out"])


def both(tmp_path, folder, fmt, cli_args, call, **kw):
    """havne-cli export and the package's call over folder: the files
    byte for byte, the counts the line's"""
    ext = {"pcapng": ".pcapng", "csv": ".csv", "folder": ""}[fmt]
    line = line_of(cli("export", folder, tmp_path / f"cli{ext}", "--format", fmt, *cli_args),
                   "exported")
    got = call(folder, tmp_path / f"py{ext}", **kw)
    assert got.path == str(tmp_path / f"py{ext}")
    assert tree(tmp_path / f"py{ext}") == tree(tmp_path / f"cli{ext}")
    same_counts(got, line)
    return got


# --- pcapng -------------------------------------------------------------

def test_a_scrubbed_pcapng_is_the_clis(tmp_path, car):
    got = both(tmp_path, car, "pcapng", [], havne.export_pcapng)
    assert got.scrubbed and got.frames > 0 and got.values == 0 and got.rows == 0
    assert got.missing == ()


def test_a_pcapng_leaves_out_what_the_scrub_finds(tmp_path):
    got = both(tmp_path, FINDINGS, "pcapng", [], havne.export_pcapng)
    assert got.left_out.unattributable > 0 or got.left_out.identification > 0
    assert isinstance(got.left_out, havne.LeftOut)


def test_a_raw_pcapng_is_the_clis(tmp_path):
    raw = both(tmp_path, FINDINGS, "pcapng", ["--raw"], havne.export_pcapng, raw=True)
    assert not raw.scrubbed and raw.left_out == (0, 0, 0, 0)
    scrubbed = havne.export_pcapng(FINDINGS, tmp_path / "scrubbed.pcapng")
    assert scrubbed.frames < raw.frames


def test_a_pcapng_over_a_range_is_the_clis(tmp_path):
    # The findings fixture spans seconds (the replayed car's recording, a
    # few ms), so a range there leaves both ends out
    lo, hi = middle(FINDINGS)
    assert lo > 0
    got = both(tmp_path, FINDINGS, "pcapng", ["--from", seconds(lo), "--to", seconds(hi)],
               havne.export_pcapng, from_us=lo, to_us=hi)
    whole = havne.export_pcapng(FINDINGS, tmp_path / "whole.pcapng")
    assert 0 < got.frames < whole.frames
    early = havne.export_pcapng(FINDINGS, tmp_path / "early.pcapng", to_us=lo)
    late = havne.export_pcapng(FINDINGS, tmp_path / "late.pcapng", from_us=hi)
    assert 0 < early.frames < whole.frames and 0 < late.frames < whole.frames


# --- the folder copy ----------------------------------------------------

def test_a_scrubbed_folder_copy_is_the_clis(tmp_path):
    got = both(tmp_path, FINDINGS, "folder", [], havne.export_folder)
    assert got.files > 0 and got.format == "folder"
    assert sorted(tree(tmp_path / "py")) == sorted(tree(tmp_path / "cli"))


def test_a_folder_copy_over_a_range_is_the_clis(tmp_path):
    lo, hi = middle(FINDINGS)
    got = both(tmp_path, FINDINGS, "folder", ["--from", seconds(lo), "--to", seconds(hi)],
               havne.export_folder, from_us=lo, to_us=hi)
    assert got.frames > 0


def test_a_raw_folder_copy_is_the_clis(tmp_path, car):
    got = both(tmp_path, car, "folder", ["--raw"], havne.export_folder, raw=True)
    assert not got.scrubbed and got.left_out == (0, 0, 0, 0)


# --- the CSV ------------------------------------------------------------

def test_a_wide_csv_of_the_recordings_own_columns_is_the_clis(tmp_path, car):
    got = both(tmp_path, car, "csv", [], havne.export_csv)
    assert got.rows > 0 and got.signal_count > 0 and got.missing == ()


def test_a_wide_csv_of_named_columns_names_the_missing_one(tmp_path, car):
    names = picks(car) + ["No Such Signal"]
    line = line_of(cli("export", car, tmp_path / "cli.csv", "--format", "csv",
                       *[a for n in names for a in ("--signal", n)]), "exported")
    got = havne.export_csv(car, tmp_path / "py.csv", signals=names)
    assert tree(tmp_path / "py.csv") == tree(tmp_path / "cli.csv")
    same_counts(got, line)
    assert got.missing == ("No Such Signal",) and line["missing"] == 1
    assert got.signal_count == 3
    with open(tmp_path / "py.csv", encoding="utf-8") as f:
        head = f.readline().rstrip("\r\n").split(",")
    assert head[0] == "Time (ms)" and head[1].startswith(names[0])
    assert head[2].startswith(names[1]) and head[3].startswith("No Such Signal")


def test_a_csv_on_a_grid_in_imperial_is_the_clis(tmp_path, car):
    got = both(tmp_path, car, "csv", ["--rows", "1", "--units", "imperial"],
               havne.export_csv, row_ms=1, units="imperial")
    assert got.rows > 0
    metric = havne.export_csv(car, tmp_path / "metric.csv", row_ms=1)
    assert (tmp_path / "metric.csv").read_bytes() != (tmp_path / "py.csv").read_bytes()
    assert metric.rows == got.rows


def test_a_csv_over_a_range_is_the_clis(tmp_path):
    lo, hi = middle(FINDINGS)
    got = both(tmp_path, FINDINGS, "csv", ["--from", seconds(lo), "--to", seconds(hi)],
               havne.export_csv, from_us=lo, to_us=hi)
    whole = havne.export_csv(FINDINGS, tmp_path / "whole.csv")
    assert 0 < got.rows < whole.rows
    sub = tmp_path / "g"
    sub.mkdir()
    grid = both(sub, FINDINGS, "csv", ["--rows", "1000", "--from", seconds(lo)],
                havne.export_csv, row_ms=1000, from_us=lo)
    assert grid.rows > 0


def test_the_long_csv_is_the_clis(tmp_path, car):
    got = both(tmp_path, car, "csv", ["--layout", "long"], havne.export_csv, layout="long")
    assert got.values > 0 and got.rows == 0
    with open(tmp_path / "py.csv", encoding="utf-8") as f:
        f.readline()
        assert f.readline().rstrip("\r\n") == "time_s,signal,unit,value"


def test_a_text_signal_is_left_out_of_a_csv_and_refused_as_a_column(tmp_path, values_only):
    got = both(tmp_path, values_only, "csv", ["--layout", "long"], havne.export_csv,
               layout="long")
    assert got.left_out.text_values > 0
    with pytest.raises(havne.Error) as e:
        havne.export_csv(values_only, tmp_path / "text.csv", signals=["Gear"])
    assert e.value.status == havne.c.ECUIO_ERR_INVALID_ARGUMENT
    assert e.value.call == "ecuio_export_with"
    assert nothing_made(tmp_path, "text.csv")


def test_a_recording_of_values_alone_exports_a_csv_and_refuses_frames(tmp_path, values_only):
    got = havne.export_csv(values_only, tmp_path / "v.csv")
    assert got.rows > 0
    assert havne.export_folder(values_only, tmp_path / "v").files >= 1


def test_the_columns_stay_in_the_order_asked(tmp_path, car):
    a, b = picks(car)
    for names in ([a, b], [b, a]):
        got = havne.export_csv(car, tmp_path / f"{'-'.join(n[:3] for n in names)}.csv",
                               signals=names)
        assert got.missing == ()
    one = (tmp_path / f"{a[:3]}-{b[:3]}.csv").read_bytes()
    two = (tmp_path / f"{b[:3]}-{a[:3]}.csv").read_bytes()
    assert one != two


# --- the sizing ---------------------------------------------------------

def test_the_sizing_is_the_clis_and_each_size_is_the_files(tmp_path, car):
    line = line_of(cli("export", car, "--format", "csv", "--estimate"), "estimate")
    est = havne.estimate_csv(car)
    assert isinstance(est, havne.Estimate) and isinstance(est.sizes[0], havne.EstimatedSize)
    assert est.columns == line["columns"] and est.missing == ()
    assert [(s.row_ms, s.rows, s.bytes) for s in est.sizes] == \
        [(s["row_ms"], s["rows"], s["bytes"]) for s in line["sizes"]]
    assert [s.row_ms for s in est.sizes] == [0, 100, 250]
    for s in est.sizes:
        out = tmp_path / f"{s.row_ms}.csv"
        got = havne.export_csv(car, out, row_ms=s.row_ms)
        assert (got.rows, os.path.getsize(out)) == (s.rows, s.bytes)


def test_the_sizing_takes_the_exports_keywords_and_names_the_missing(tmp_path, car):
    a, b = picks(car)
    shape = dict(signals=[a, "No Such Signal", b], units="imperial")
    lo, hi = middle(car)
    est = havne.estimate_csv(car, row_ms=[500, 50, 500], from_us=lo, to_us=hi, **shape)
    assert est.missing == ("No Such Signal",)
    assert [s.row_ms for s in est.sizes] == [500, 50, 500], "in the order asked, repeats too"
    assert est.sizes[0] == est.sizes[2]
    args = [x for n in shape["signals"] for x in ("--signal", n)]
    line = line_of(cli("export", car, "--format", "csv", "--estimate", "--rows", "50",
                       "--units", "imperial", "--from", seconds(lo), "--to", seconds(hi), *args),
                   "estimate")
    assert (est.sizes[1].rows, est.sizes[1].bytes) == (line["sizes"][0]["rows"],
                                                       line["sizes"][0]["bytes"])
    got = havne.export_csv(car, tmp_path / "s.csv", row_ms=50, from_us=lo, to_us=hi, **shape)
    assert os.path.getsize(tmp_path / "s.csv") == est.sizes[1].bytes
    assert got.missing == est.missing


def test_one_interval_is_a_sizing_of_one(car):
    est = havne.estimate_csv(car, row_ms=250)
    assert [s.row_ms for s in est.sizes] == [250]


# --- arguments ----------------------------------------------------------

def refused(kind, call, *args, **kw):
    with pytest.raises(kind):
        call(*args, **kw)


def test_a_wrong_argument_is_refused_before_core_is_called(tmp_path, car, monkeypatch):
    calls = []
    for name in ("ecuio_export_with", "ecuio_export_estimate_with", "ecuio_recording_open"):
        monkeypatch.setattr(havne.export.lib, name, lambda *a, **k: calls.append(a))
    out = tmp_path / "x"
    v, t = ValueError, TypeError
    for kind, call, kw in [
        (v, havne.export_csv, dict(layout="tall")),
        (v, havne.export_csv, dict(units="si")),
        (t, havne.export_csv, dict(layout=1)),
        (v, havne.export_csv, dict(row_ms=-1)),
        (v, havne.export_csv, dict(row_ms=86_400_001)),
        (t, havne.export_csv, dict(row_ms=1.5)),
        (t, havne.export_csv, dict(row_ms=True)),
        (t, havne.export_csv, dict(signals="RPM")),
        (t, havne.export_csv, dict(signals=[1])),
        (v, havne.export_csv, dict(signals=[])),
        (v, havne.export_csv, dict(signals=[""])),
        (v, havne.export_csv, dict(layout="long", signals=["RPM"])),
        (v, havne.export_csv, dict(layout="long", row_ms=100)),
        (v, havne.export_csv, dict(layout="long", units="imperial")),
        (v, havne.export_csv, dict(to_us=0)),
        (v, havne.export_csv, dict(from_us=5, to_us=4)),
        (v, havne.export_csv, dict(from_us=-1)),
        (t, havne.export_csv, dict(from_us=1.5)),
        (t, havne.export_csv, dict(from_us=True)),
        (t, havne.export_csv, dict(to_us="9")),
        (t, havne.export_csv, dict(cancel=3)),
        (t, havne.export_csv, dict(raw=1)),
        (v, havne.export_pcapng, dict(to_us=0)),
        (v, havne.export_pcapng, dict(from_us=5, to_us=4)),
        (t, havne.export_pcapng, dict(cancel="stop")),
        (v, havne.export_folder, dict(to_us=0)),
        (t, havne.export_folder, dict(raw="yes")),
    ]:
        refused(kind, call, car, out, **kw)
    for kw, kind in [(dict(row_ms=[]), v), (dict(row_ms=list(range(65))), v),
                     (dict(row_ms=[100, 86_400_001]), v), (dict(row_ms=[-1]), v),
                     (dict(row_ms="100"), t), (dict(row_ms=[1.5]), t), (dict(row_ms=None), t),
                     (dict(layout="long"), t), (dict(signals="RPM"), t), (dict(signals=[]), v),
                     (dict(units="si"), v), (dict(to_us=0), v), (dict(cancel=1), t)]:
        refused(kind, havne.estimate_csv, car, **kw)
    refused(t, havne.export_csv, car, None)
    refused(t, havne.export_pcapng, None, out)
    assert calls == [], "core was never called"
    assert nothing_made(tmp_path, "x")


def test_a_range_may_be_one_instant_and_paths_may_be_path_objects(tmp_path, car):
    lo = max(middle(car)[0], 1)   # to_us 0 is refused, and a recording can be short
    got = havne.export_pcapng(os.fspath(car), tmp_path / "one.pcapng", from_us=lo, to_us=lo)
    assert got.path == str(tmp_path / "one.pcapng") and os.path.exists(got.path)
    import pathlib
    assert havne.export_pcapng(pathlib.Path(car), pathlib.Path(tmp_path) / "two.pcapng")


def test_an_open_recording_hands_its_folder_on(tmp_path, car):
    with havne.Recording(car) as r:
        assert r.folder == str(car)
        assert havne.export_csv(r.folder, tmp_path / "r.csv").rows > 0


# --- core's refusals ----------------------------------------------------

def test_core_refuses_what_it_refuses_and_leaves_what_was_there(tmp_path, car):
    out = tmp_path / "taken.csv"
    out.write_text("mine", encoding="utf-8")
    with pytest.raises(havne.Error) as e:
        havne.export_csv(car, out)
    assert e.value.status == havne.c.ECUIO_ERR_LOAD_FAILED
    assert e.value.call == "ecuio_export_with" and "exists" in e.value.detail
    assert not isinstance(e.value, havne.CheckFailed)
    assert out.read_text(encoding="utf-8") == "mine" and not partials(tmp_path, "taken.csv")
    with pytest.raises(havne.Error) as e:
        havne.export_pcapng(tmp_path / "nowhere", tmp_path / "n.pcapng")
    assert e.value.status == havne.c.ECUIO_ERR_LOAD_FAILED
    assert nothing_made(tmp_path, "n.pcapng")
    with pytest.raises(havne.Error) as e:
        havne.estimate_csv(tmp_path / "nowhere")
    assert e.value.call == "ecuio_export_estimate_with"
    with pytest.raises(havne.Error) as e:
        havne.export_csv(tmp_path / "nowhere", tmp_path / "n.csv", signals=["RPM"])
    assert e.value.call == "ecuio_export_with", "the open that finds none leaves it to the export"
    with pytest.raises(havne.Error) as e:
        havne.export_csv(car, tmp_path / "dup.csv", signals=picks(car)[:1] * 2)
    assert e.value.status == havne.c.ECUIO_ERR_INVALID_ARGUMENT


@pytest.mark.parametrize("call", [
    lambda f, o, **kw: havne.export_pcapng(f, o, **kw),
    lambda f, o, **kw: havne.export_folder(f, o, **kw),
])
def test_a_failed_check_is_check_failed_with_its_class_and_writes_nothing(
        tmp_path, echoes_vin, call):
    with pytest.raises(havne.CheckFailed) as e:
        call(echoes_vin, tmp_path / "out")
    assert isinstance(e.value, havne.Error) and e.value.failed_class == "identification"
    assert e.value.status == havne.c.ECUIO_ERR_LOAD_FAILED
    assert "ZZZFAKE" not in str(e.value) and "ZZZFAKE" not in e.value.detail
    assert nothing_made(tmp_path, "out")
    line_of(cli("export", echoes_vin, tmp_path / "cli", "--format", "pcapng", status=3),
            "export_failed")
    raw = call(echoes_vin, tmp_path / "raw", raw=True)
    assert not raw.scrubbed


def test_each_failed_class_has_its_word():
    from havne.export import class_word
    assert [class_word(i) for i in range(0, 8)] == \
        ["unknown", "identification", "unattributable", "text_values", "activity", "vin_shaped",
         "unknown", "unknown"]


# --- the cancel ---------------------------------------------------------

def run_pcapng(tmp_path, name, cancel, folder):
    return havne.export_pcapng(folder, tmp_path / name, cancel=cancel)


def run_folder(tmp_path, name, cancel, folder):
    return havne.export_folder(folder, tmp_path / name, cancel=cancel)


def run_csv(tmp_path, name, cancel, folder):
    return havne.export_csv(folder, tmp_path / name, cancel=cancel)


def run_csv_named(tmp_path, name, cancel, folder):
    return havne.export_csv(folder, tmp_path / name, signals=picks(folder), cancel=cancel)


def run_long(tmp_path, name, cancel, folder):
    return havne.export_csv(folder, tmp_path / name, layout="long", cancel=cancel)


def run_estimate(tmp_path, name, cancel, folder):
    return havne.estimate_csv(folder, cancel=cancel)


def run_estimate_named(tmp_path, name, cancel, folder):
    return havne.estimate_csv(folder, signals=picks(folder), cancel=cancel)


RUNS = [run_pcapng, run_folder, run_csv, run_csv_named, run_long, run_estimate,
        run_estimate_named]
WRITES = [r for r in RUNS if "estimate" not in r.__name__]


@pytest.fixture(params=RUNS, ids=lambda r: r.__name__[4:])
def run(request, car):
    return lambda tmp_path, name, cancel: request.param(tmp_path, name, cancel, car)


def made_nothing(tmp_path, name):
    """Nothing at name and nothing beside it: the sizing never makes a file"""
    return nothing_made(tmp_path, name) and not os.listdir(tmp_path)


def test_a_cancel_that_never_stops_gives_the_same_result(tmp_path, run):
    asks = Asks()
    with_cancel = run(tmp_path, "with", asks)
    without = run(tmp_path, "without", None)
    assert asks.asked >= 2
    if isinstance(with_cancel, havne.Exported):
        assert with_cancel._replace(path="") == without._replace(path="")
        assert tree(tmp_path / "with") == tree(tmp_path / "without")
    else:
        assert with_cancel == without


def test_a_stop_at_each_ask_in_turn_leaves_nothing(tmp_path, run):
    whole = Asks()
    run(tmp_path, "whole", whole)
    for at in range(1, whole.asked + 1):
        asks = Asks(at)
        with pytest.raises(havne.Error) as e:
            run(tmp_path, f"stop-{at}", asks)
        assert e.value.status == havne.c.ECUIO_ERR_CANCELED
        # The reader's open says only its status's words; the export its own
        assert "canceled" in (e.value.detail or e.value.words)
        assert e.value.call in ("ecuio_recording_open", "ecuio_export_with",
                                "ecuio_export_estimate_with")
        assert asks.asked == at, "asked no more once it said stop"
        assert nothing_made(tmp_path, f"stop-{at}")
        assert not [n for n in os.listdir(tmp_path) if ".partial-" in n]


def test_an_exception_in_the_cancel_reaches_the_caller(tmp_path, run):
    asks = Asks(1, RuntimeError("the caller's own"))
    with pytest.raises(RuntimeError, match="the caller's own"):
        run(tmp_path, "raised", asks)
    assert asks.asked == 1 and nothing_made(tmp_path, "raised")
    asks = Asks(2, KeyboardInterrupt())
    with pytest.raises(KeyboardInterrupt):
        run(tmp_path, "interrupted", asks)
    assert asks.asked == 2 and nothing_made(tmp_path, "interrupted")


def test_a_ctrl_c_stops_the_call_at_its_next_ask(tmp_path, run):
    during = []

    def ctrl_c():
        during.append(signal.getsignal(signal.SIGINT))
        signal.raise_signal(signal.SIGINT)
        return False

    asks = Asks(1, ctrl_c)
    with pytest.raises(KeyboardInterrupt):
        run(tmp_path, "ctrl-c", asks)
    assert asks.asked == 1 and nothing_made(tmp_path, "ctrl-c")
    assert during[0] is not signal.default_int_handler
    assert signal.getsignal(signal.SIGINT) is signal.default_int_handler


def test_a_call_off_the_main_thread_leaves_the_handler_alone(tmp_path, run):
    got = []
    asks = Asks()
    thread = threading.Thread(target=lambda: got.append(run(tmp_path, "thread", asks)))
    thread.start()
    thread.join()
    assert len(got) == 1 and asks.asked >= 2
    assert signal.getsignal(signal.SIGINT) is signal.default_int_handler


def test_a_stop_during_the_readers_open_writes_nothing(tmp_path, car):
    # The first ask of a named wide CSV is the reader's open, which finds
    # the missing columns first: the same cancel, the same handler
    asks = Asks(1)
    with pytest.raises(havne.Error) as e:
        havne.export_csv(car, tmp_path / "open.csv", signals=picks(car), cancel=asks)
    assert e.value.status == havne.c.ECUIO_ERR_CANCELED and e.value.call == "ecuio_recording_open"
    assert asks.asked == 1 and nothing_made(tmp_path, "open.csv")
    asks = Asks(1, RuntimeError("in the open"))
    with pytest.raises(RuntimeError, match="in the open"):
        havne.estimate_csv(car, signals=picks(car), cancel=asks)
    # Core asks again once the export itself begins
    whole = Asks()
    havne.export_csv(car, tmp_path / "w.csv", signals=picks(car), cancel=whole)
    plain = Asks()
    havne.export_csv(car, tmp_path / "p.csv", cancel=plain)
    assert whole.asked > plain.asked, "the open's asks come before the export's"


def test_a_ctrl_c_with_no_cancel_at_all_still_stops_the_call(tmp_path, car):
    # The handler is swapped in for every call, whether or not a cancel is
    # given: a Ctrl+C just before core is entered is read at its first ask
    def interrupt(*_):
        signal.raise_signal(signal.SIGINT)

    real = havne.export.lib.ecuio_export_with
    state = {"n": 0}

    class Spy:
        def __call__(self, options, callback, user, report):
            state["n"] += 1
            interrupt()
            return real(options, callback, user, report)

    havne.export.lib.ecuio_export_with = Spy()
    try:
        with pytest.raises(KeyboardInterrupt):
            havne.export_pcapng(car, tmp_path / "late.pcapng")
    finally:
        havne.export.lib.ecuio_export_with = real
    assert state["n"] == 1 and nothing_made(tmp_path, "late.pcapng")
    assert signal.getsignal(signal.SIGINT) is signal.default_int_handler


def test_the_columns_memory_outlives_the_call(tmp_path, car):
    # The array of names is held across core's call: a collected one would
    # give core garbage names (an empty or a wrong column)
    import gc
    gc.collect()
    got = havne.export_csv(car, tmp_path / "keep.csv", signals=picks(car))
    assert got.missing == () and got.signal_count == 2
