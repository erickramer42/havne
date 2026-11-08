# File/layer: python/tests/test_import.py | pytest
# Testing: HARNESS, PythonTest.test_import: a logger's CSV and a candump
#          log imported through the package and read back
#
# The made-up loggers' CSVs (tests/fixtures/import/csv) imported by their
# column maps, as a dict and as JSON text, then opened with Recording; a
# candump log imported and its frames decoded through obd2; refusals carry
# core's words and leave no folder. The same CSVs' maps guessed from their
# headers (each its committed guessed-map.json) and imported without a map;
# a time with no unit imported only with time_unit. An OBDb set converted
# through the declarations, its text freed by core.

import ctypes
import json
import os

import pytest

import havne
from havne._core import lib
from conftest import ROOT

CSV = os.path.join(ROOT, "tests", "fixtures", "import", "csv")
LOG = (
    "(1700000000.250000) can0 7DF#0201000000000000\n"
    "(1700000000.254000) can0 7E8#04410C1AF8\n"
    "(1700000000.350000) can1 7E8#04410C0FA0\n"
)


def test_an_obdb_import_through_the_declarations_is_freed_by_core():
    # The converted file is core's to free: its pointer comes back as
    # core gave it, and the text is read from it before the free
    with open(os.path.join(ROOT, "tests", "fixtures", "obdb", "example-car.json"), "rb") as f:
        data = f.read()
    out, length = ctypes.POINTER(ctypes.c_char)(), ctypes.c_size_t()
    rc = lib.ecuio_obdb_import(data, len(data), None, havne.c.ecuio_obdb_note_fn(), None,
                               ctypes.byref(out), ctypes.byref(length))
    assert rc == havne.c.ECUIO_OK
    try:
        converted = ctypes.string_at(out, length.value)
    finally:
        lib.ecuio_obdb_free(out)
    assert json.loads(converted)["havne_signals"] == 1
    havne.SignalFile(converted)


def column_map(name):
    with open(os.path.join(CSV, name, "map.json"), encoding="utf-8") as f:
        return json.load(f)


def test_a_loggers_csv_imports_by_its_map_and_reads_back(tmp_path):
    folder = tmp_path / "logger-a"
    got = havne.import_csv(os.path.join(CSV, "logger-a", "log.csv"), folder,
                           column_map("logger-a"))
    assert got == havne.Imported(str(folder), 11, 0, 0, 0, 22, 1, None, 1_750_000)
    assert sorted(os.listdir(folder)) == ["events.log", "signals.csv"]
    with havne.Recording(folder) as r:
        assert r.info.utc_start_us == 0
        assert [s.name for s in r.signals] == ["RPM", "Engine Load", "Lambda"]
        rpm = r.signal("RPM")
        assert (rpm.samples, rpm.lowest, rpm.highest, rpm.unit) == (8, 780.5, 3420.25, "rpm")
        assert r.text_at("Lambda", 1_000_000) == ("0.86", 1_000_000)


def test_a_map_as_json_text_and_iso_times_give_the_utc_start(tmp_path):
    folder = tmp_path / "logger-b"
    text = json.dumps(column_map("logger-b"))
    got = havne.import_csv(os.path.join(CSV, "logger-b", "log.csv"), folder, text)
    assert (got.values, got.columns_ignored, got.utc_start_us) == (16, 2, 1793883802250000)
    with havne.Recording(folder) as r:
        assert r.info.utc_start_us == 1793883802250000
        assert r.signal("Gear").kind == "text"
        assert r.text_at("Gear", 3_500_000) == ("3", 3_500_000)


def test_a_refused_map_raises_cores_words_and_makes_nothing(tmp_path):
    folder = tmp_path / "refused"
    bad = {**column_map("logger-a"), "shade": "red"}
    with pytest.raises(havne.Error) as e:
        havne.import_csv(os.path.join(CSV, "logger-a", "log.csv"), folder, bad)
    assert e.value.status == havne.c.ECUIO_ERR_LOAD_FAILED
    assert "'shade'" in e.value.detail
    assert not os.path.exists(folder) and not os.path.exists(str(folder) + ".partial")
    with pytest.raises(TypeError):
        havne.import_csv(os.path.join(CSV, "logger-a", "log.csv"), folder, 3)


def test_a_candump_log_imports_and_decodes_through_obd2(tmp_path):
    log = tmp_path / "bus.log"
    log.write_text(LOG, encoding="utf-8", newline="\n")
    with pytest.raises(havne.Error) as e:
        havne.import_candump(log, tmp_path / "both")
    assert "can0 and can1" in e.value.detail
    assert not os.path.exists(tmp_path / "both")
    folder = tmp_path / "rec"
    got = havne.import_candump(log, folder, interface="can0")
    assert (got.frames, got.interfaces, got.values, got.utc_start_us,
            got.duration_us) == (2, 2, 0, 1700000000250000, 4000)
    with havne.Recording(folder, profile="obd2") as r:
        rpm = r.signal("RPM")
        assert (rpm.samples, rpm.lowest) == (1, 1726.0)


def guessed_map(name):
    with open(os.path.join(CSV, name, "guessed-map.json"), encoding="utf-8") as f:
        return json.load(f)


def test_a_csvs_map_is_guessed_from_its_header():
    g = havne.guess_csv_map(os.path.join(CSV, "logger-a", "log.csv"))
    assert g.map == guessed_map("logger-a")
    assert g.flags == (havne.c.ECUIO_CSV_GUESS_TIME_UNIT_KNOWN |
                       havne.c.ECUIO_CSV_GUESS_DECIMAL_COMMA | havne.c.ECUIO_CSV_GUESS_PREAMBLE |
                       havne.c.ECUIO_CSV_GUESS_TIME_NUMBERS)
    assert "the header is line 3: the lines above it are left out" in g.notes
    assert [c["column"] for c in g.columns] == ["Time [s]", "Engine Speed [rpm]", "Load [%]",
                                               "Lambda", "Note"]
    assert all(c["used"] for c in g.columns)


def test_without_a_map_the_guess_is_imported(tmp_path):
    folder = tmp_path / "logger-b"
    got = havne.import_csv(os.path.join(CSV, "logger-b", "log.csv"), folder)
    assert (got.values, got.columns_ignored, got.utc_start_us) == (27, 0, 1793883802250000)
    with havne.Recording(folder) as r:
        assert [s.name for s in r.signals] == ["speed_kmh", "gear", "throttle", "comment"]
        assert r.text_at("gear", 3_500_000) == ("3", 3_500_000)


def test_a_time_with_no_unit_imports_only_with_one(tmp_path):
    plain = tmp_path / "plain.csv"
    plain.write_text("time,A\n0,1\n1,2\n", encoding="utf-8", newline="\n")
    g = havne.guess_csv_map(plain)
    assert "unit" not in g.map["time"]
    assert not g.flags & havne.c.ECUIO_CSV_GUESS_TIME_UNIT_KNOWN
    assert g.flags & havne.c.ECUIO_CSV_GUESS_TIME_NUMBERS
    assert "'time' is the time, its unit not known: say s, ms or us" in g.notes
    folder = tmp_path / "rec"
    with pytest.raises(havne.Error) as e:
        havne.import_csv(plain, folder)
    assert e.value.status == havne.c.ECUIO_ERR_LOAD_FAILED
    assert "time_unit" in e.value.detail
    assert not os.path.exists(folder)
    assert havne.guess_csv_map(plain, time_unit="us").map["time"]["unit"] == "us"
    got = havne.import_csv(plain, folder, time_unit="ms")
    assert (got.values, got.duration_us) == (2, 1000)


def test_a_time_no_unit_can_read_names_a_map(tmp_path):
    zoneless = tmp_path / "zoneless.csv"
    zoneless.write_text("timestamp,A\n2026-11-05T22:03:22.250,1\n2026-11-05T22:03:22.750,2\n",
                        encoding="utf-8", newline="\n")
    g = havne.guess_csv_map(zoneless, time_unit="ms")
    assert not g.flags & (havne.c.ECUIO_CSV_GUESS_TIME_UNIT_KNOWN |
                          havne.c.ECUIO_CSV_GUESS_TIME_NUMBERS)
    folder = tmp_path / "rec"
    with pytest.raises(havne.Error) as e:
        havne.import_csv(zoneless, folder, time_unit="ms")
    assert "time_unit" not in e.value.detail and "column_map" in e.value.detail
    assert not os.path.exists(folder)


def test_a_guess_refused_says_why(tmp_path):
    numbers = tmp_path / "numbers.csv"
    numbers.write_text("1,2\n3,4\n", encoding="utf-8", newline="\n")
    with pytest.raises(havne.Error) as e:
        havne.guess_csv_map(numbers)
    assert e.value.status == havne.c.ECUIO_ERR_LOAD_FAILED
    assert e.value.detail.startswith("no row within the file's first 1000")
    with pytest.raises(havne.Error):
        havne.import_csv(numbers, tmp_path / "rec")
    assert not os.path.exists(tmp_path / "rec")
    log = os.path.join(CSV, "logger-a", "log.csv")
    with pytest.raises(ValueError):
        havne.guess_csv_map(log, time_unit="h")
    with pytest.raises(ValueError):
        havne.import_csv(log, tmp_path / "rec", column_map("logger-a"), time_unit="s")
