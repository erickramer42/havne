# File/layer: tests/cli_conditions_drop.py | test-only, Python 3.8+
# Testing: HARNESS (ctest CliConditionsRunTest.ADroppedEventIsCountedOnTheEndLine):
#          havne-cli stream's conditions_dropped (ADR-0063)
#
#   python tests/cli_conditions_drop.py <havne-cli>
#
# A live stream whose reader stops reading drops lines, and a condition
# event whose line is dropped is counted on the end line. mock://demo
# idles fast from about 17 s to 34 s, its engine speed hunting a few rpm
# on each tick; 64 conditions on it, levels 0.75 rpm apart across the
# idle's band, give many events a second. The line ring is at its floor
# (HAVNE_CLI_RING_BYTES) and the stream records: the recorder's ring is
# its own, so the recording's cond lines are every event the session
# gave. Read to 17 s of session time, then stop reading until the stream
# must be dropping, whatever the system's buffers hold and however
# loaded the machine is:
# - the bytes the kernel holds for the reader stop growing, so the
#   child's writes wait;
# - the ring (and the batch the writer took from it) is full once the
#   cond lines alone, at the least a line of them takes, outweigh what
#   the pipe and the ring hold: counted in events of the recording,
#   never in time, which load stretches;
# - the recording, written live, holds PAST_FULL events past those,
#   and each of them is a dropped line.
# Read to the end; then the lines received plus conditions_dropped are
# the cond lines, at least PAST_FULL dropped, and each line received is
# one of them. Linux's pipe is cut to a page, and on macOS (whose pipe
# can't be cut) the child writes to a Unix socket pair that asks for
# 4 KiB buffers: both shorten the stall where the system keeps to them.

import collections
import ctypes
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

SECONDS = 33
STALL_FROM_US = 17 * 1000 * 1000
STALL_MAX_S = 25.0
STILL_S = 0.5
POLL_S = 0.05
FILL_BYTES = 2 * (32 * 1024 + 256)
# More than the kernel holds for the reader on any platform (a page of
# pipe on Linux, 4 KiB socket buffers on macOS, Windows' small default):
# a stall seen before the child's writes blocked is covered
HELD_MAX_BYTES = 16 * 1024
LEAST_COND_BYTES = 60
PAST_FULL = 3
LEVELS = 64


def condition_file(folder):
    conditions = []
    for k in range(LEVELS):
        conditions.append({
            "key": "idle_%02d" % k,
            "name": "Idle %02d" % k,
            "when": {"signal": ["id:1001"], "unit": "RPM",
                     "above": 1405.875 + 0.75 * k},
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
    the ones that fill the pipe and the ring: (events needed, held bytes,
    words why not)"""
    begun = time.monotonic()
    deadline = begun + STALL_MAX_S
    held = held_bytes(fd)
    still_since = time.monotonic()
    while time.monotonic() - still_since < STILL_S:
        if time.monotonic() > deadline:
            return 0, held, "the kernel's buffer never stopped growing"
        time.sleep(POLL_S)
        now = held_bytes(fd)
        if now != held:
            held, still_since = now, time.monotonic()
    # Cond lines alone, each at least cond_bytes, outweigh the kernel's
    # buffer and the ring, so the events after them are dropped lines
    full = -(-(HELD_MAX_BYTES + FILL_BYTES) // cond_bytes)
    while True:
        folders = os.listdir(root)
        after = 0
        if len(folders) == 1:
            log = os.path.join(root, folders[0], "events.log")
            if os.path.exists(log):
                after = len([e for e in cond_events(log) if e[4] >= stall_us])
        if after >= full + PAST_FULL:
            return full, held, ""
        if time.monotonic() > deadline:
            return full, held, (f"{after} events recorded from "
                                f"{stall_us / 1e6:.1f} s, {full + PAST_FULL} "
                                f"needed")
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
        proc = subprocess.Popen(
            [cli, "stream", "mock://demo", "--values", "--conditions", named,
             "--record", root, "--seconds", str(SECONDS)],
            stdin=subprocess.DEVNULL,
            stdout=theirs.fileno() if theirs else subprocess.PIPE,
            stderr=subprocess.PIPE, env=env)
        if theirs:
            theirs.close()
            reader = ours.makefile("rb")
        else:
            reader = proc.stdout
            if pipe_cut:
                fcntl.fcntl(reader.fileno(), fcntl.F_SETPIPE_SZ, 4096)
        held_fd = ours.fileno() if ours else reader.fileno()
        lines = []
        stalled = None
        shortest = None
        for raw in reader:
            line = json.loads(raw)
            lines.append(line)
            if line.get("type") == "condition":
                shortest = min(shortest or len(raw), len(raw))
            t = line.get("t_us")
            if t is None:
                continue
            if stalled is None and t >= STALL_FROM_US:
                cond_bytes = max(int((shortest or 0) * 0.9), LEAST_COND_BYTES)
                stalled = stall(held_fd, root, t, cond_bytes)
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
              f"dropped_values {end.get('dropped_values')}")
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
