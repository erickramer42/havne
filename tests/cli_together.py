import hashlib
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

OBD2_RPM = "7E8:410C:2.0:16"
PART_RPM = "7E8:62F40C:3.0:16"


def run(cli, args, env=None):
    done = subprocess.run([cli] + args, capture_output=True, env=env,
                          timeout=120)
    rows = [json.loads(l) for l in done.stdout.decode("utf-8").splitlines()
            if l.strip()]
    return done.returncode, rows, done.stderr.decode("utf-8", "replace")


def of_type(rows, kind):
    return [r for r in rows if r.get("type") == kind]


def fail(what):
    print(f"FAIL  {what}")
    return 1


def merged_name(base, parts):
    text = "".join(p + "\n" for p in ([base] if base else []) + parts)
    return "merged." + hashlib.sha256(text.encode()).hexdigest()[:16]


def modules(cli, fx):
    trans = str(fx / "trans-part.json")
    for word, base in (("obd2-modules,", "obd2 modules"), ("", None)):
        code, rows, err = run(cli, [
            "stream", "mock://x", "--together", word + "user.trans-part",
            "--signals", trans, "--values", "--count", "1"])
        session = (of_type(rows, "session") or [{}])[0]
        signals = (of_type(rows, "signals") or [{}])[0].get("signals", [])
        want = merged_name(base, ["user.trans-part"])
        names = [s["name"] for s in signals]
        if code != 0 or session.get("merged") != want or \
                names != ["Trans fluid temp"]:
            return fail(f"{word or 'no base'}: exit {code}, "
                        f"{session.get('merged')} not {want}, {names}\n{err}")
    print("ok    obd2-modules reads obd2's modules and none of its signals")
    return 0


def notes(cli, fx):
    code, rows, err = run(cli, [
        "stream", "mock://x", "--together",
        "obd2,user.engine-part,user.trans-part",
        "--signals", str(fx / "engine-part.json"),
        "--signals", str(fx / "trans-part.json"), "--count", "1"])
    if code != 0 or len(rows) < 3:
        return fail(f"exit {code}, {rows}\n{err}")
    session, note = rows[1], rows[2]
    want = ["obd2", "user.engine-part", "user.trans-part"]
    if session.get("type") != "session" or session.get("together") != want or \
            session.get("merged") != session.get("profile") or \
            not session["profile"].startswith("merged."):
        return fail(f"the session line {session}")
    if note != {"type": "note", "kind": "beside_base",
                "text": "'RPM' (Engine part) is read beside obd2's 'RPM': "
                        "a merged set replaces no obd2 signal"}:
        return fail(f"the note line {note}")
    code, rows, err = run(cli, ["stream", "mock://x", "--count", "1"])
    session = (of_type(rows, "session") or [{}])[0]
    if "together" in session or "merged" in session or of_type(rows, "note"):
        return fail(f"a plain stream's lines {session}")
    print("ok    the session line names the merge and a note line follows")
    return 0


def shared(cli, fx):
    with tempfile.TemporaryDirectory(prefix="havne-cli-together-") as root:
        code, rows, err = run(cli, [
            "log", "mock://x", root, "--together", "obd2,user.engine-part",
            "--signals", str(fx / "engine-part.json"), "--signal", "RPM",
            "--seconds", "1"])
    words = (f"2 signals are named 'RPM': name one by its key "
             f"({OBD2_RPM}, {PART_RPM})")
    if code != 2 or rows or words not in err:
        return fail(f"exit {code}, {rows}, {err!r}")
    print("ok    a name two signals have is usage listing both keys")
    return 0


class Emulator:
    def __init__(self, cli, extra):
        self.p = subprocess.Popen(
            [cli, "sim", "mock://sim?values=canned", "--elm327", "tcp:0",
             "--seconds", "60"] + extra,
            stdout=subprocess.PIPE, text=True)
        self.port = None
        for line in self.p.stdout:
            j = json.loads(line)
            if j["type"] == "elm327":
                self.port = int(j["listen"].rsplit(":", 1)[1])
                return
            if j["type"] == "error":
                raise AssertionError(f"the emulator didn't start: {j}")
        raise AssertionError("no elm327 line")

    def end(self):
        self.p.terminate()
        self.p.wait(timeout=60)


def ranked(cli, fx):
    together = ["--together", "obd2,user.engine-part",
                "--signals", str(fx / "engine-part.json")]
    emulator = Emulator(cli, together)
    try:
        elm = (f"elm327://127.0.0.1:{emulator.port}"
               "?identify=0&diagnose=0&probe=0")
        with tempfile.TemporaryDirectory(prefix="havne-cli-together-") as root:
            logged = run(cli, ["log", elm, root, "--pick", "ranked",
                               "--seconds", "2"] + together)
        streamed = run(cli, ["stream", elm, "--pick", "ranked",
                             "--seconds", "1"] + together)
    finally:
        emulator.end()
    for what, (code, rows, err) in (("log", logged), ("stream", streamed)):
        focus = (of_type(rows, "focus") or [{}])[0]
        keys = [s.get("key") for s in focus.get("signals", [])]
        if code != 0 or focus.get("pick") != "ranked" or \
                PART_RPM not in keys or OBD2_RPM in keys:
            return fail(f"{what}'s ranked focus {keys}, exit {code}\n{err}")
    read = [r for r in of_type(logged[1], "interval") if r.get("key") == PART_RPM]
    if not read or read[0].get("values", 0) == 0:
        return fail(f"the emulator didn't serve the merged set: {read}")
    print("ok    the ranked pick leaves out the obd2 signal read beside")
    return 0


def out(cli, fx):
    args = ["plugins", "together", "--together", "obd2-modules,user.trans-part",
            "--signals", str(fx / "trans-part.json")]
    with tempfile.TemporaryDirectory(prefix="havne-cli-together-") as root:
        written = Path(root) / "merged.json"
        code, rows, err = run(cli, args + ["--out", str(written)])
        totals = (of_type(rows, "totals") or [{}])[0]
        ended = (of_type(rows, "end") or [{}])[0]
        if code != 0 or ended.get("out") != str(written) or \
                totals.get("base") != "modules" or \
                totals.get("profile") != merged_name("obd2 modules", ["user.trans-part"]):
            return fail(f"the run: exit {code}, {rows}\n{err}")
        code, rows, err = run(cli, ["signals", "check", str(written)])
        read = (of_type(rows, "signal_file") or [{}])[0]
        if code != 0 or read.get("profile") != totals["profile"]:
            return fail(f"the written set reads back as {read}, exit {code}\n{err}")
        code, rows, err = run(cli, args + ["--out", str(Path(root) / "no" / "x.json")])
        if code != 1 or not of_type(rows, "error") or of_type(rows, "end"):
            return fail(f"a write that fails: exit {code}, {rows}")
    print("ok    plugins together writes the merged set's text")
    return 0


def view(cli, fx):
    together = ["--together", "user.trans-part",
                "--signals", str(fx / "trans-part.json")]
    with tempfile.TemporaryDirectory(prefix="havne-cli-together-") as root:
        code, rows, err = run(cli, ["log", "mock://x", root, "--seconds", "1"]
                              + together)
        ended = (of_type(rows, "end") or [{}])[0]
        if code != 0 or not ended.get("folder"):
            return fail(f"the log: exit {code}, {rows}\n{err}")
        (Path(ended["folder"]) / "signals.json").unlink()
        code, rows, err = run(cli, ["view", ended["folder"], "--frames"]
                              + together)
    opened = (of_type(rows, "opened") or [{}])[0]
    want = merged_name(None, ["user.trans-part"])
    if code != 0 or opened.get("frames") != want:
        return fail(f"view --frames: exit {code}, {opened}\n{err}")
    print("ok    view --frames decodes through --together's set")
    return 0


ROOM = {
    "havne_signals": 1, "name": "Room", "source": {"name": "t"},
    "ecus": [{"id": "m1", "rx": "7E9", "name": "M1"},
             {"id": "m2", "rx": "7EA", "name": "M2"},
             {"id": "m3", "rx": "7EB", "name": "M3"},
             {"id": "door", "rx": "7EC"}],
    "signals": [{"key": f"s{i}", "name": f"N s{i}", "ecu": e,
                 "read": {"service": "22", "number": f"100{i - 1}"},
                 "field": {"byte": 0}}
                for i, e in ((1, "m1"), (2, "m2"), (3, "m3"), (4, "door"))],
}


def noroom(cli, fx, dll):
    if os.name != "nt":
        return 77
    env = dict(os.environ)
    env.update({"ECUIO_FAKE_MODULE_DIDS":
                "7E9:1000=11;7EA:1001=12;7EB:1002=13;7EC:1003=14",
                "ECUIO_FAKE_FILTER_GATING": "1", "ECUIO_FAKE_FILTER_MAX": "3"})
    with tempfile.TemporaryDirectory(prefix="havne-cli-together-") as root:
        room = Path(root) / "room.json"
        room.write_text(json.dumps(ROOM), encoding="utf-8")
        args = ["log", f"j2534://{dll}?identify=0&diagnose=0",
                str(Path(root) / "out"), "--signals", str(room), "--seconds",
                "5"]
        for i in range(1, 5):
            args += ["--signal", f"N s{i}"]
        code, rows, err = run(cli, args, env)
    words = ("havne-cli: logging 1 signal the adapter has no room for: "
             "DOOR's N s4: their columns stay empty")
    lines = [l for l in err.splitlines() if "no room" in l]
    if code != 0 or lines != [words]:
        return fail(f"exit {code}, {lines}\n{err}")
    print("ok    log names the signals the adapter has no room for")
    return 0


def main():
    case, cli, fx = sys.argv[1], sys.argv[2], Path(sys.argv[3])
    if case == "noroom":
        return noroom(cli, fx, sys.argv[4])
    return {"modules": modules, "notes": notes, "shared": shared,
            "ranked": ranked, "view": view, "out": out}[case](cli, fx)


if __name__ == "__main__":
    sys.exit(main())
