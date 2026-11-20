# File/layer: tests/cli_transfer_interrupt.py | test-only, Python 3.8+, no packages
# Testing: HARNESS (ctest CliTransferRunTest.interrupt, .interrupt-twice,
#          .interrupt-late): havne-cli export, import and health report
#          stopped by an interrupt
#
#   python tests/cli_transfer_interrupt.py interrupt <havne-cli> <recording>
#   python tests/cli_transfer_interrupt.py interrupt-twice <havne-cli> <recording>
#   python tests/cli_transfer_interrupt.py interrupt-late <havne-cli> <recording>
#
# interrupt: an export to pcapng, a folder copy, a CSV's sizing, a wide
# CSV and its sizing during their named columns' check, a wide CSV at an
# interval during its sweep's sizing, an import of a candump log and of a
# CSV, and a recording's health report, each held at its first ask
# (HAVNE_CLI_EXPORT_HOLD_MS, which names that ask on stderr: the wide
# CSVs' must be the check's, the interval's the interval check's, an
# import's the import's, the report's the health report's) and stopped by
# Ctrl+Break or SIGINT once held, end within a few seconds with an
# interrupted end line and exit 1, nothing at the file or folder named and
# no partial beside it; exit 77 where there's no console to send the event
# through.

import json
import os
import signal
import subprocess
import sys
import tempfile
import threading
import time

from cli_transfer_kit import LOG, partial_left


# Long enough that a stop the export never asks about fails the case
HOLD_MS = 20000
# How long a run may take to reach its first ask and say it holds there
HELD_WAIT_S = 30


# The run's deadline after its second interrupt
TWICE_WITHIN_S = 2


def twice(cli, args, where):
    """args run held at the first ask, deaf to the first interrupt
    (HAVNE_CLI_EXPORT_HOLD_DEAF), then interrupted twice half a second
    apart: the second ends the process at once, exit 1, its last line the
    end line. None, a failure's words, or "skip" with no console"""
    env = dict(os.environ)
    env["HAVNE_CLI_EXPORT_HOLD_MS"] = str(HOLD_MS)
    env["HAVNE_CLI_EXPORT_HOLD_DEAF"] = "1000"
    kw = {"stdout": subprocess.PIPE, "stderr": subprocess.PIPE,
          "stdin": subprocess.DEVNULL, "env": env}
    if os.name == "nt":
        kw["creationflags"] = subprocess.CREATE_NEW_PROCESS_GROUP
    p = subprocess.Popen([cli] + args, **kw)
    held = []
    reader = threading.Thread(target=lambda: held.append(p.stderr.readline()))
    reader.start()
    reader.join(HELD_WAIT_S)
    if reader.is_alive() or not held or \
            not held[0].startswith(b"havne-cli: holding the " + where + b"'s"):
        p.kill()
        p.communicate()
        reader.join()
        return f"didn't hold at the {where.decode()} within {HELD_WAIT_S} s"
    interrupt_with = signal.CTRL_BREAK_EVENT if os.name == "nt" else signal.SIGINT
    try:
        p.send_signal(interrupt_with)
        time.sleep(0.5)
        if p.poll() is not None:
            return f"the first interrupt ended it (exit {p.returncode}): the hold isn't deaf"
        p.send_signal(interrupt_with)
    except OSError as e:
        p.kill()
        p.communicate()
        print(f"skip  no console to send the interrupt through: {e}")
        return "skip"
    sent = time.monotonic()
    try:
        out, err = p.communicate(timeout=TWICE_WITHIN_S)
    except subprocess.TimeoutExpired:
        p.kill()
        p.communicate()
        return f"still running {TWICE_WITHIN_S} s after the second interrupt"
    lines = [json.loads(l) for l in out.decode("utf-8").splitlines()]
    if p.returncode != 1 or not lines or \
            lines[-1] != {"type": "end", "reason": "interrupted"}:
        return (f"exit {p.returncode}, lines {lines}, "
                f"{err.decode('utf-8', 'replace')}")
    print(f"ok    {case_name(args)}: a second interrupt ended it "
          f"{time.monotonic() - sent:.2f} s after it")
    return None


def interrupt_twice(cli, recording):
    with tempfile.TemporaryDirectory(prefix="havne-cli-int2-") as root:
        log = os.path.join(root, "bus.log")
        with open(log, "w", encoding="utf-8", newline="\n") as f:
            f.write(LOG)
        cases = [
            (["import", "candump", log, os.path.join(root, "from-log")], b"import"),
            (["export", recording, os.path.join(root, "out.pcapng"),
              "--format", "pcapng"], b"export"),
            (["health", recording, os.path.join(root, "report.json"),
              "--format", "json"], b"health report"),
        ]
        failed = 0
        for args, where in cases:
            why = twice(cli, args, where)
            if why == "skip":
                return 77
            if why is not None:
                print(f"FAIL  {case_name(args)}: {why}")
                failed += 1
    if failed:
        return 1
    print("ok    a second interrupt ends an export, an import and a "
          "recording's health report at once")
    return 0


# How long a run waits once core's call has returned
TAIL_MS = 3000


def late(cli, args, where, result, made):
    """args run to the end of core's call and held there
    (HAVNE_CLI_EXPORT_TAIL_HOLD_MS), then interrupted twice: the run is
    done, so it ends as it would have, its result line whole, exit 0, no
    end line. None, a failure's words, or "skip" with no console"""
    env = dict(os.environ)
    env["HAVNE_CLI_EXPORT_TAIL_HOLD_MS"] = str(TAIL_MS)
    kw = {"stdout": subprocess.PIPE, "stderr": subprocess.PIPE,
          "stdin": subprocess.DEVNULL, "env": env}
    if os.name == "nt":
        kw["creationflags"] = subprocess.CREATE_NEW_PROCESS_GROUP
    p = subprocess.Popen([cli] + args, **kw)
    held = []
    reader = threading.Thread(target=lambda: held.append(p.stderr.readline()))
    reader.start()
    reader.join(HELD_WAIT_S)
    if reader.is_alive() or not held or \
            not held[0].startswith(b"havne-cli: holding after the " + where):
        p.kill()
        out, err = p.communicate()
        reader.join()
        return (f"didn't hold after the {where.decode()}'s call within "
                f"{HELD_WAIT_S} s: exit {p.returncode}, "
                f"{(held[0] if held else b'') + err!r}")
    interrupt_with = signal.CTRL_BREAK_EVENT if os.name == "nt" else signal.SIGINT
    try:
        p.send_signal(interrupt_with)
        time.sleep(0.3)
        if p.poll() is None:
            p.send_signal(interrupt_with)
    except OSError as e:
        p.kill()
        p.communicate()
        print(f"skip  no console to send the interrupt through: {e}")
        return "skip"
    try:
        out, err = p.communicate(timeout=TAIL_MS / 1000 + 20)
    except subprocess.TimeoutExpired:
        p.kill()
        p.communicate()
        return "still running long after its hold"
    lines = [json.loads(l) for l in out.decode("utf-8").splitlines()]
    types = [l["type"] for l in lines]
    if p.returncode != 0 or types != ["start", result]:
        return (f"exit {p.returncode}, lines {lines}, "
                f"{err.decode('utf-8', 'replace')}")
    if made is not None and (not os.path.exists(made) or partial_left(made)):
        return f"{os.path.basename(made)} isn't whole in place"
    print(f"ok    {case_name(args)}: two interrupts after core's call "
          f"changed nothing")
    return None


def canceled_late(cli, recording, root):
    """A column check canceled by the first interrupt, then held before the
    end line (HAVNE_CLI_EXPORT_TAIL_HOLD_MS) and interrupted again: the
    second interrupt changes nothing, so the run waits out its hold and
    writes the one interrupted end line, exit 1, nothing made. None, a
    failure's words, or "skip" with no console"""
    out_csv = os.path.join(root, "late-check.csv")
    env = dict(os.environ)
    env["HAVNE_CLI_EXPORT_HOLD_MS"] = str(HOLD_MS)
    env["HAVNE_CLI_EXPORT_TAIL_HOLD_MS"] = str(TAIL_MS)
    kw = {"stdout": subprocess.PIPE, "stderr": subprocess.PIPE,
          "stdin": subprocess.DEVNULL, "env": env}
    if os.name == "nt":
        kw["creationflags"] = subprocess.CREATE_NEW_PROCESS_GROUP
    p = subprocess.Popen([cli, "export", recording, out_csv, "--format",
                          "csv", "--signal", "Engine RPM"], **kw)
    interrupt_with = signal.CTRL_BREAK_EVENT if os.name == "nt" else signal.SIGINT

    def next_line(prefix):
        got = []
        reader = threading.Thread(
            target=lambda: got.append(p.stderr.readline()))
        reader.start()
        reader.join(HELD_WAIT_S)
        if reader.is_alive() or not got or not got[0].startswith(prefix):
            p.kill()
            p.communicate()
            reader.join()
            return None, (f"no {prefix.decode()!r} within {HELD_WAIT_S} s: "
                          f"{got[0] if got else b''!r}")
        return got[0], None

    _, why = next_line(b"havne-cli: holding the column check's first ask")
    if why:
        return why
    try:
        p.send_signal(interrupt_with)
    except OSError as e:
        p.kill()
        p.communicate()
        print(f"skip  no console to send the interrupt through: {e}")
        return "skip"
    _, why = next_line(b"havne-cli: holding after the column check's call")
    if why:
        return why
    held_at = time.monotonic()
    p.send_signal(interrupt_with)
    try:
        out, err = p.communicate(timeout=TAIL_MS / 1000 + 20)
    except subprocess.TimeoutExpired:
        p.kill()
        p.communicate()
        return "still running long after its hold"
    waited = time.monotonic() - held_at
    lines = [json.loads(l) for l in out.decode("utf-8").splitlines()]
    if p.returncode != 1 or [l["type"] for l in lines] != ["start", "end"]             or lines[1] != {"type": "end", "reason": "interrupted"}:
        return (f"exit {p.returncode}, lines {lines}, "
                f"{err.decode('utf-8', 'replace')}")
    if waited < TAIL_MS / 1000 - 0.5:
        return (f"ended {waited:.2f} s into a {TAIL_MS / 1000:.0f} s hold: "
                f"the second interrupt ended it")
    if os.path.exists(out_csv) or partial_left(out_csv):
        return f"something was left beside {os.path.basename(out_csv)}"
    print("ok    export --signal: a second interrupt after the canceled "
          "column check changed nothing")
    return None


def interrupt_late(cli, recording):
    with tempfile.TemporaryDirectory(prefix="havne-cli-int3-") as root:
        log = os.path.join(root, "bus.log")
        with open(log, "w", encoding="utf-8", newline="\n") as f:
            f.write(LOG)
        cases = [
            (["import", "candump", log, os.path.join(root, "from-log")],
             b"import", "imported", os.path.join(root, "from-log")),
            (["export", recording, os.path.join(root, "out.csv"),
              "--format", "csv"], b"export", "exported",
             os.path.join(root, "out.csv")),
            (["export", recording, "--format", "csv", "--estimate"],
             b"estimate", "estimate", None),
            (["health", recording, os.path.join(root, "report.json"),
              "--format", "json"], b"health report", "health",
             os.path.join(root, "report.json")),
        ]
        failed = 0
        for args, where, result, made in cases:
            why = late(cli, args, where, result, made)
            if why == "skip":
                return 77
            if why is not None:
                print(f"FAIL  {case_name(args)}: {why}")
                failed += 1
        why = canceled_late(cli, recording, root)
        if why == "skip":
            return 77
        if why is not None:
            print(f"FAIL  export --signal: {why}")
            failed += 1
    if failed:
        return 1
    print("ok    interrupts after core's call leave an export, its sizing, "
          "an import and a recording's health report as they ended")
    return 0


def interrupted(cli, args, where):
    """args run held at the first ask, which must be where's (b"export",
    b"column check", b"interval check", b"import" or b"health report"),
    then interrupted:
    None, a failure's words, or "skip" with no console"""
    env = dict(os.environ)
    env["HAVNE_CLI_EXPORT_HOLD_MS"] = str(HOLD_MS)
    kw = {"stdout": subprocess.PIPE, "stderr": subprocess.PIPE,
          "stdin": subprocess.DEVNULL, "env": env}
    if os.name == "nt":
        kw["creationflags"] = subprocess.CREATE_NEW_PROCESS_GROUP
    p = subprocess.Popen([cli] + args, **kw)
    held = []
    reader = threading.Thread(target=lambda: held.append(p.stderr.readline()))
    reader.start()
    reader.join(HELD_WAIT_S)
    if reader.is_alive() or not held or \
            not held[0].startswith(b"havne-cli: holding the " + where + b"'s"):
        p.kill()
        out, err = p.communicate()
        reader.join()
        return (f"didn't hold at the {where.decode()} within "
                f"{HELD_WAIT_S} s: exit "
                f"{p.returncode}, {(held[0] if held else b'') + err!r}")
    try:
        p.send_signal(signal.CTRL_BREAK_EVENT if os.name == "nt"
                      else signal.SIGINT)
    except OSError as e:
        p.kill()
        p.communicate()
        print(f"skip  no console to send the interrupt through: {e}")
        return "skip"
    sent = time.monotonic()
    try:
        out, err = p.communicate(timeout=HOLD_MS / 2000)
    except subprocess.TimeoutExpired:
        p.kill()
        p.communicate()
        return f"still running {HOLD_MS / 2000:.0f} s after the interrupt"
    took = time.monotonic() - sent
    lines = [json.loads(l) for l in out.decode("utf-8").splitlines()]
    if p.returncode != 1 or not lines or \
            lines[-1] != {"type": "end", "reason": "interrupted"}:
        return (f"exit {p.returncode}, lines {lines}, "
                f"{err.decode('utf-8', 'replace')}")
    if any(l["type"] in ("exported", "estimate", "imported", "health",
                         "health_failed", "error")
           for l in lines):
        return f"an interrupted run says more than its end: {lines}"
    print(f"ok    {case_name(args)}: interrupted, ended {took:.2f} s after it")
    return None


def case_name(args):
    """The command and its options, or the import's kind"""
    return args[0] + " " + " ".join(args[args.index("--format"):]) \
        if "--format" in args else " ".join(args[:2])


def interrupt(cli, recording):
    """Each long command interrupted at its first ask. Core has made
    nothing by then, so the leftover check here is that first ask's alone;
    what a stop at each later ask leaves is
    ExportCancelTest.AStopAtEachAskInTurnLeavesNothingBehind's and
    ImportCancelTest's"""
    with tempfile.TemporaryDirectory(prefix="havne-cli-int-") as root:
        log = os.path.join(root, "bus.log")
        with open(log, "w", encoding="utf-8", newline="\n") as f:
            f.write(LOG)
        values = os.path.join(root, "values.csv")
        with open(values, "w", encoding="utf-8", newline="\n") as f:
            f.write("t,Engine Speed\n0,800\n1,810\n")
        cmap = os.path.join(root, "map.json")
        with open(cmap, "w", encoding="utf-8", newline="\n") as f:
            json.dump({"havne_column_map": 1,
                       "time": {"column": "t", "unit": "s"},
                       "signals": [{"column": "Engine Speed", "name": "RPM",
                                    "unit": "rpm"}]}, f)
        cases = [
            (["import", "candump", log, os.path.join(root, "from-log")],
             os.path.join(root, "from-log"), b"import"),
            (["import", "csv", values, os.path.join(root, "from-csv"),
              "--map", cmap], os.path.join(root, "from-csv"), b"import"),
            (["export", recording, os.path.join(root, "out.pcapng"),
              "--format", "pcapng"], os.path.join(root, "out.pcapng"),
             b"export"),
            (["export", recording, os.path.join(root, "copy"),
              "--format", "folder"], os.path.join(root, "copy"), b"export"),
            (["export", recording, "--format", "csv", "--estimate"], None,
             b"export"),
            (["export", recording, os.path.join(root, "out.csv"),
              "--format", "csv", "--signal", "Engine RPM"],
             os.path.join(root, "out.csv"), b"column check"),
            (["export", recording, "--format", "csv", "--estimate",
              "--signal", "Engine RPM"], None, b"column check"),
            # an interval: the sweep's sizing before the export holds
            (["export", recording, os.path.join(root, "grid.csv"),
              "--format", "csv", "--rows", "100"],
             os.path.join(root, "grid.csv"), b"interval check"),
            (["health", recording, os.path.join(root, "report.json"),
              "--format", "json"], os.path.join(root, "report.json"),
             b"health report"),
        ]
        failed = 0
        for args, made, where in cases:
            why = interrupted(cli, args, where)
            if why == "skip":
                return 77
            if why is None and made is not None and (
                    os.path.exists(made) or partial_left(made)):
                why = (f"something was left at or beside "
                       f"{os.path.basename(made)}: {os.listdir(root)}")
            if why is not None:
                print(f"FAIL  {case_name(args)}: {why}")
                failed += 1
    if failed:
        return 1
    print("ok    an interrupt stops an export, its sizing, an import and a "
          "recording's health report, leaving nothing")
    return 0




if __name__ == "__main__":
    if len(sys.argv) == 4 and sys.argv[1] == "interrupt":
        sys.exit(interrupt(sys.argv[2], sys.argv[3]))
    if len(sys.argv) == 4 and sys.argv[1] == "interrupt-twice":
        sys.exit(interrupt_twice(sys.argv[2], sys.argv[3]))
    if len(sys.argv) == 4 and sys.argv[1] == "interrupt-late":
        sys.exit(interrupt_late(sys.argv[2], sys.argv[3]))
    print(__doc__ or "usage: cli_transfer_interrupt.py interrupt|interrupt-twice|interrupt-late <havne-cli> <argument>")
    sys.exit(2)
