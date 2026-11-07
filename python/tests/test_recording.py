# File/layer: python/tests/test_recording.py | pytest
# Testing: HARNESS, PythonTest.test_recording: a session recorded and read
#          back through the package
#
# A mock:// session recorded through Session.record, a note and a marker
# on the way, read back with Recording: its signals with their samples, the
# marker, a query, the value at a time (signals.csv's last row), a
# threshold's crossing; then the same folder decoded again from its
# frames.

import csv
import os
import time

import pytest

import havne


def recorded(tmp_path, seconds=1.2):
    with havne.Session("mock://py") as s:
        rec = s.record(tmp_path, label="py test")
        s.start()
        time.sleep(seconds / 2)
        rec.mark("half way")
        rec.note("a note from the script")
        time.sleep(seconds / 2)
        folder = rec.folder
    return folder


def last_row(folder, signal):
    """(value, host_us) of the signal's last row in signals.csv"""
    with open(os.path.join(folder, "signals.csv"), encoding="utf-8-sig", newline="") as f:
        rows = [r for r in csv.DictReader(f) if r["signal"] == signal]
    return rows[-1]["value"], int(rows[-1]["host_us"])


def test_a_recorded_session_reads_back(tmp_path):
    folder = recorded(tmp_path)
    assert folder.startswith(str(tmp_path).replace("\\", "/")) or folder.startswith(str(tmp_path))
    assert folder.endswith("py test")
    with havne.Recording(folder) as r:
        assert r.info.duration_us > 1_000_000
        rpm = r.signal("RPM")
        assert rpm.kind == "number" and rpm.samples > 10
        assert rpm.unit == "RPM"
        assert [m.label for m in r.markers] == ["half way"]
        assert r.find_marker("half way") == r.markers[0].host_us
        assert r.find_marker("no such marker") is None
        cols = r.query(["RPM", "Vehicle Speed"], 0, r.info.duration_us, 4)
        assert len(cols) == 2 and all(len(c) == 4 for c in cols)
        assert sum(c.samples for c in cols[0]) == rpm.samples
        value, at = r.text_at("RPM", rpm.last_us)
        assert (value, at) == last_row(folder, "RPM")
        assert at == rpm.last_us
        assert r.text_at("RPM", 0)[1] in (None, 0)
        coolant = r.signal("Coolant Temp")
        assert r.find_threshold("Coolant Temp", coolant.highest + 1000) is None


def test_a_recording_decodes_again_from_its_frames(tmp_path):
    folder = recorded(tmp_path, 0.8)
    with havne.Recording(folder) as plain, havne.Recording(folder, profile="mock") as again:
        assert again.info.flags & havne.c.ECUIO_RECORDING_INFO_FRAMES
        assert again.signal("RPM").samples == plain.signal("RPM").samples


def test_a_folder_that_isn_t_a_recording_is_refused(tmp_path):
    with pytest.raises(havne.Error) as e:
        havne.Recording(tmp_path)
    assert e.value.name == "LOAD_FAILED"
    assert "signals.csv" in e.value.detail
    assert str(tmp_path) not in e.value.detail, "never the path"
