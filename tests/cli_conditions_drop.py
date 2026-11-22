# File/layer: tests/cli_conditions_drop.py | test-only, Python 3.8+
# Testing: HARNESS (ctest CliConditionsRunTest.ADroppedEventIsCountedOnTheEndLine):
#          havne-cli stream's conditions_dropped (ADR-0063)
#
#   python tests/cli_conditions_drop.py <havne-cli>
#
# A live stream whose reader stops reading drops lines, and a condition
# event whose line is dropped is counted on the end line. mock://demo
# holds the rev limiter from its 1018th tick to its 1118th (5 s of the
# drive cycle, about 50.9 s to 55.9 s while the worker keeps 20 ticks a
# second), its engine speed sawing between about 6860 and 7000 rpm on each
# tick; 64 conditions on it, levels 2.5 rpm apart across that band, cross
# several dozen levels on every tick, so the events come with the
# session's ticks, not with the machine's speed. The drive cycle moves a
# tick at a time and a worker that falls a whole interval behind starts
# its schedule over (mock_transport.cpp), so on a loaded runner the cycle
# runs behind the wall clock: the stall and the run's end are counted in
# frames (ticks), never in seconds. The line ring is at its floor
# (HAVNE_CLI_RING_BYTES) and the stream records: the recorder's ring is
# its own, so the recording's cond lines are every event the session
# gave. Read to STALL_AT_FRAMES frames, then stop reading until the stream
# must be dropping, whatever the system's buffers hold and however
# loaded the machine is:
# - the cond lines since the stall go to the reader's buffer, the pipe,
#   the ring (and the batch the writer took from it), or are dropped;
# - the recording, written live, counts them, and the bytes the kernel
#   holds, read after each count, bound the pipe's share: they only grow
#   while nothing reads, so a child starved or blocked reads the same;
# - once the cond lines, at the least a line of them takes, outweigh all
#   those bytes by PAST_FULL lines, each of those is a dropped line:
#   counted in events, never in time, which load stretches.
# Read to the end; then the lines received plus conditions_dropped are
# the cond lines, at least PAST_FULL dropped, and each line received is
# one of them. Linux's pipe is cut to a page, and on macOS (whose pipe
# can't be cut) the child writes to a Unix socket pair that asks for
# 4 KiB buffers: both shorten the stall where the system keeps to them.

import collections
import ctypes
import io
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time

try:
    import fcntl
    import termios
except ImportError:
    fcntl = termios = None

# --count ends the run on the worker's ticks; --seconds is only the bound
# for a run that has wedged
COUNT = 1250
SECONDS = 150
STALL_AT_FRAMES = 1000
STALL_MAX_S = 25.0
POLL_S = 0.05
# The ring at its floor and the batch the writer took from it
FILL_BYTES = 2 * (32 * 1024 + 256)
# The lines the reader's buffer took past the stall's frame, received later
READ_AHEAD_BYTES = io.DEFAULT_BUFFER_SIZE
LEAST_COND_BYTES = 60
PAST_FULL = 3
LEVELS = 64
# The limiter crosses the band's levels about 110 times in each three
# ticks, and holds it for 100 ticks: thousands of events a loop, where the fill
# is about 600 (the pipe's and the ring's bytes over a cond line's). A long
# key (the limit is 31 bytes) makes each cond line longer, so fewer events
# fill them
LEVEL_FROM_RPM = 6855.0
LEVEL_STEP_RPM = 2.5
KEY_PREFIX = "idle_level_at_rpm_threshold_"


def condition_file(folder):
    conditions = []
    for k in range(LEVELS):
        conditions.append({
            "key": KEY_PREFIX + "%02d" % k,
            "name": "Idle %02d" % k,
            "when": {"signal": ["id:1001"], "unit": "RPM",
                     "above": LEVEL_FROM_RPM + LEVEL_STEP_RPM * k},
        })
    path = os.path.join(folder, "idle.json")
    with open(path, "w", encoding="utf-8") as f:
        json.dump({"havne_conditions": 1, "name": "Idle levels",
                   "conditions": conditions}, f)
    return path


def cond_events(events_log):
    """(key, state, fired, stale, t_us) for each whole cond line"""
    out = []
    with open(events_log, encoding="utf-8") as f:
        for line in f:
            if "  cond  " not in line or not line.endswith("\n"):
                continue
            fields = line.rstrip("\n").split("  cond  ", 1)[1].split(",")
            words = fields[2].split()
            out.append((fields[0], fields[1], "fired" in words,
                        "stale" in words, int(fields[6])))
    return out


def held_bytes(fd):
    """The bytes the kernel holds for the reader on fd"""
    if os.name == "nt":
        import msvcrt
        avail = ctypes.c_ulong(0)
        if not ctypes.windll.kernel32.PeekNamedPipe(
                ctypes.c_void_p(msvcrt.get_osfhandle(fd)), None, 0, None,
                ctypes.byref(avail), None):
            raise OSError("PeekNamedPipe failed")
        return avail.value
    n = ctypes.c_int(0)
    fcntl.ioctl(fd, termios.FIONREAD, n)
    return n.value


def stall(fd, root, stall_us, cond_bytes):
    """Holds the reading until the recording has PAST_FULL events past
    the ones that fill the reader's buffer, the pipe and the ring:
    (events needed, held bytes, words why not)"""
    begun = time.monotonic()
    deadline = begun + STALL_MAX_S
    while True:
        folders = os.listdir(root)
        after = 0
        if len(folders) == 1:
            log = os.path.join(root, folders[0], "events.log")
            if os.path.exists(log):
                after = len([e for e in cond_events(log) if e[4] >= stall_us])
        # Read after the count: with nothing read, the held bytes only grow,
        # so these are at least the ones held when the counted events came
        held = held_bytes(fd)
        full = -(-(held + FILL_BYTES + READ_AHEAD_BYTES) // cond_bytes)
        if after >= full + PAST_FULL:
            return full, held, ""
        if time.monotonic() > deadline:
            spent = time.monotonic() - begun
            return full, held, (f"{after} events recorded from "
                                f"{stall_us / 1e6:.1f} s of session time, {full + PAST_FULL} "
                                f"needed, in {spent:.1f} s of waiting "
                                f"({after / spent:.0f} events a second), "
                                f"cond lines of {cond_bytes} B")
        time.sleep(POLL_S)


def main():
    cli = os.path.abspath(sys.argv[1])
    scratch = tempfile.mkdtemp(prefix="havne-cli-conditions-drop-")
    try:
        named = condition_file(scratch)
        root = os.path.join(scratch, "recordings")
        os.mkdir(root)
        env = dict(os.environ, HAVNE_CLI_RING_BYTES="1")
        pipe_cut = fcntl is not None and hasattr(fcntl, "F_SETPIPE_SZ")
        ours = theirs = None
        if os.name == "posix" and not pipe_cut:
            ours, theirs = socket.socketpair()
            for side in (ours, theirs):
                side.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 4096)
                side.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
        cut_r = cut_w = None
        if pipe_cut:
            # Cut before the child exists: Linux refuses (EBUSY) to shrink a
            # pipe already holding more than the new size
            cut_r, cut_w = os.pipe()
            fcntl.fcntl(cut_w, fcntl.F_SETPIPE_SZ, 4096)
        proc = subprocess.Popen(
            [cli, "stream", "mock://demo", "--values", "--conditions", named,
             "--record", root, "--count", str(COUNT), "--seconds",
             str(SECONDS)],
            stdin=subprocess.DEVNULL,
            stdout=(theirs.fileno() if theirs else
                    cut_w if pipe_cut else subprocess.PIPE),
            stderr=subprocess.PIPE, env=env)
        if theirs:
            theirs.close()
            reader = ours.makefile("rb")
        elif pipe_cut:
            os.close(cut_w)
            reader = os.fdopen(cut_r, "rb")
        else:
            reader = proc.stdout
        held_fd = ours.fileno() if ours else reader.fileno()
        lines = []
        stalled = None
        shortest = None
        frames = 0
        stall_us = 0
        begun = time.monotonic()
        for raw in reader:
            line = json.loads(raw)
            lines.append(line)
            if line.get("type") == "condition":
                shortest = min(shortest or len(raw), len(raw))
            if line.get("type") != "frame":
                continue
            frames += 1
            if stalled is None and frames >= STALL_AT_FRAMES:
                stall_us = line["t_us"]
                cond_bytes = max(int((shortest or 0) * 0.9), LEAST_COND_BYTES)
                spent = time.monotonic() - begun
                print(f"      {frames} frames in {spent:.1f} s of waiting, "
                      f"session time {stall_us / 1e6:.1f} s: "
                      f"{frames / max(stall_us / 1e6, 0.001):.1f} ticks a "
                      f"second")
                stalled = stall(held_fd, root, stall_us, cond_bytes)
                print(f"      the stall: the kernel held {stalled[1]} B, "
                      f"{stalled[0]} cond lines of {cond_bytes} B or more "
                      f"fill the ring")
        err = proc.stderr.read().decode(errors="replace")
        proc.wait(timeout=60)
        end = [l for l in lines if l.get("type") == "end"]
        recording = [l for l in lines if l.get("type") == "recording"
                     and l.get("state") == "closed"]
        if proc.returncode != 0 or len(end) != 1 or len(recording) != 1 \
                or stalled is None:
            print(f"FAIL  the run: exit {proc.returncode}, stalled "
                  f"{stalled is not None}, {len(end)} end and "
                  f"{len(recording)} recording lines: {err}")
            return 1
        if stalled[2]:
            print(f"FAIL  the stall never made the stream drop: {stalled[2]}")
            return 1
        end, recording = end[0], recording[0]
        ok = True
        if not recording.get("whole") or "conditions_lost" in recording:
            print(f"FAIL  the recording lost events: {recording}")
            ok = False
        folders = os.listdir(root)
        if len(folders) != 1:
            print(f"FAIL  {len(folders)} recordings")
            return 1
        truth = cond_events(os.path.join(root, folders[0], "events.log"))
        got = [(l["key"], l["state"], l["fired"], l["stale"], l["t_us"])
               for l in lines if l.get("type") == "condition"]
        dropped = end.get("conditions_dropped")
        print(f"      {len(got)} condition lines received, "
              f"conditions_dropped {dropped}, {len(truth)} cond lines "
              f"recorded; dropped_frames {end.get('dropped_frames')}, "
              f"dropped_values {end.get('dropped_values')}; "
              f"{len([e for e in truth if e[4] >= stall_us])} recorded "
              f"from {stall_us / 1e6:.1f} s on")
        if dropped is None:
            print("FAIL  the end line has no conditions_dropped")
            ok = False
        elif len(got) + dropped != len(truth):
            print(f"FAIL  {len(got)} received + {dropped} dropped is not "
                  f"the {len(truth)} recorded")
            ok = False
        elif dropped < PAST_FULL:
            print(f"FAIL  {dropped} condition events dropped: the "
                  f"{PAST_FULL} recorded after the ring was full are all "
                  f"dropped")
            ok = False
        missing = collections.Counter(got) - collections.Counter(truth)
        if missing:
            print(f"FAIL  lines received that no cond line holds: "
                  f"{list(missing)[:5]}")
            ok = False
        if ok:
            print("ok    every event is a line received or counted dropped")
        return 0 if ok else 1
    finally:
        shutil.rmtree(scratch, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
