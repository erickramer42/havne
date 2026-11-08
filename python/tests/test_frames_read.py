# File/layer: python/tests/test_frames_read.py | pytest
# Testing: UNIT, PythonTest.test_frames_read: a recording's frames read
#          back through the package
#
# havne.read_frames over the replay fixture against Python's own reading
# of its frames.csv: the whole file, a stretch with both edges, a limit;
# then a refused row in core's words and an exception in the caller's
# own code passed back out.

import csv
import os

import pytest

import havne
from conftest import REPLAY


def plain_rows():
    with open(os.path.join(REPLAY, "frames.csv"), newline="", encoding="ascii") as f:
        return [havne.RecordedFrame(int(r["host_us"]), int(r["timestamp_us"]),
                                    int(r["id"], 16), int(r["flags"], 16),
                                    bytes.fromhex(r["data"]))
                for r in csv.DictReader(f)]


def test_the_whole_file_reads_as_recorded():
    rows = plain_rows()
    assert havne.read_frames(REPLAY) == rows
    assert havne.read_frames(os.path.join(REPLAY, "frames.csv")) == rows


def test_a_stretch_has_both_edges():
    rows = plain_rows()
    from_us, to_us = rows[1000].host_us, rows[2000].host_us
    got = havne.read_frames(REPLAY, from_us, to_us)
    assert got == [r for r in rows if from_us <= r.host_us <= to_us]
    assert got[0].host_us == from_us and got[-1].host_us == to_us
    assert havne.read_frames(REPLAY, from_us + 1, from_us + 2) == []


def test_a_limit_stops_the_read():
    rows = plain_rows()
    assert havne.read_frames(REPLAY, limit=3) == rows[:3]
    with pytest.raises(ValueError):
        havne.read_frames(REPLAY, limit=0)


def test_a_refusal_is_cores_words(tmp_path):
    (tmp_path / "frames.csv").write_text(
        "host_us,timestamp_us,id,flags,data\n10,10,0x7E8,0x0,41\n5,5,0x7E8,0x0,41\n",
        encoding="ascii")
    with pytest.raises(havne.Error) as caught:
        havne.read_frames(tmp_path)
    assert "host_us is earlier than the row before" in str(caught.value)
    with pytest.raises(havne.Error) as missing:
        havne.read_frames(tmp_path / "nothing-here")
    assert "the folder has no frames.csv" in str(missing.value)
    with pytest.raises(havne.Error):
        havne.read_frames(tmp_path, 20, 10)


def test_an_exception_in_the_callback_comes_out(monkeypatch):
    class Boom(Exception):
        pass

    def broken(*_args):
        raise Boom()

    monkeypatch.setattr(havne.recording, "RecordedFrame", broken)
    with pytest.raises(Boom):
        havne.read_frames(REPLAY)
