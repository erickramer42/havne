# File/layer: python/tests/test_replay.py | pytest
# Testing: HARNESS, PythonTest.test_replay: the replay fixture through
#          the package
#
# A replay's streams end at its last frame (ECUIO_ERR_STREAM_ENDED), so a
# script's for loop finishes; every frame arrives, and the values are the
# load test's (bench_bridge --load: the fixture's 1,261.25 rpm peak).

import havne

from conftest import replay_uri


def test_a_replay_s_streams_end_at_its_last_frame_with_every_frame():
    with havne.Session(replay_uri("?profile=obd2&speed=max")) as s:
        total = s.info.replay_frames
        frames = s.frames(capacity=havne.c.ECUIO_QUEUE_MAX_FRAMES)
        values = s.values(capacity=havne.c.ECUIO_QUEUE_MAX_VALUES)
        s.start()
        got = list(frames)        # ends: the replay's end, not a timeout
        rpm = [v.value for v in values if v.name == "RPM"]
    assert frames.ended == "stream ended" and values.ended == "stream ended"
    assert len(got) + frames.dropped == total
    assert values.dropped == 0
    assert max(rpm) == 1261.25


def test_a_looping_replay_runs_on_until_the_close():
    with havne.Session(replay_uri("?profile=obd2&speed=max&loop=1")) as s:
        total = s.info.replay_frames
        frames = s.frames(capacity=havne.c.ECUIO_QUEUE_MAX_FRAMES)
        s.start()
        seen = 0
        for _ in frames:
            seen += 1
            if seen + frames.dropped > 2 * total:
                break
        assert frames.ended is None, "past two passes and still going"
    assert frames.ended is None or frames.ended == "session closed"
