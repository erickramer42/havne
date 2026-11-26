# File/layer: tests/cli_consent.py | test-only, Python 3.8+, no packages
# Testing: HARNESS (ctest CliConsentTest.*): havne-cli's consent words from
#          outside the process (ADR-0066's appendix of 2026-11-23)
#
#   python tests/cli_consent.py terms <havne-cli> <talker3.json> <golden.txt>
#                                     [--update]
#   python tests/cli_consent.py store <havne-cli> <talker3.json>
#   python tests/cli_consent.py no-publisher <havne-cli> <format-2 package>
#   python tests/cli_consent.py run <havne-cli> <talker3.json>
#   python tests/cli_consent.py unlock <havne-cli> <cli_unlock_fixture>
#
# terms: `plugins terms`' text against its golden, byte for byte (line
# ends aside), the package's path written as <package>. store: give, list
# and withdraw in a scratch HAVNE_SETTINGS_DIR, whose consent.json is the
# default store; an unreadable store is exit 3 with an error line.
# no-publisher: a plugin with no publisher says it reads signals only, and
# takes no consent. run: stream on the emulator's ELM327 (sim --elm327):
# with no record exit 2, the refusal on standard error (16 digits), nothing
# on standard output and no connection (an unreadable store exit 3, the
# same nothing); with one its session line carries
# the consent; a plugin whose terms changed shows the version-change block;
# a withdrawn record refuses again. unlock: stream --unlock pcm:01 on the
# emulator with the plugins cli_unlock_fixture writes: the right key's run
# says asked once and reaches unlocked, its wire log one seed asked; the
# wrong key's first run fails, and a second inside the delay says delayed
# and asks no seed (the emulator still took its connection).

import json
import os
import subprocess
import sys
import tempfile


def run(cli, args, env=None):
    done = subprocess.run([cli] + args, capture_output=True, env=env)
    return (done.returncode,
            done.stdout.decode("utf-8").replace("\r\n", "\n"),
            done.stderr.decode("utf-8", "replace").replace("\r\n", "\n"))


def rows_of(text):
    return [json.loads(line) for line in text.splitlines() if line.strip()]


def fingerprint_of(cli, package):
    code, out, err = run(cli, ["plugins", "terms", package, "--json"])
    terms = [r for r in rows_of(out) if r.get("type") == "terms"]
    if code != 0 or len(terms) != 1:
        raise SystemExit(f"FAIL  plugins terms --json: exit {code} {err!r}")
    return terms[0]["fingerprint"]


def terms_case(cli, package, golden, update):
    code, out, err = run(cli, ["plugins", "terms", package])
    text = out.replace("\r\n", "\n").replace(package, "<package>")
    if code != 0:
        print(f"FAIL  plugins terms: exit {code} {err!r}")
        return 1
    if update:
        with open(golden, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
        print(f"wrote {golden}")
        return 0
    with open(golden, encoding="utf-8") as f:
        want = f.read()
    if text != want:
        print("FAIL  plugins terms differs from its golden:\n" + text)
        return 1
    print(f"ok    plugins terms ({len(text)} bytes)")
    return 0


def store_case(cli, package):
    print_ = fingerprint_of(cli, package)
    with tempfile.TemporaryDirectory(prefix="havne-cli-consent-") as home:
        env = dict(os.environ, HAVNE_SETTINGS_DIR=home)
        code, out, err = run(cli, ["plugins", "consent", package, "--give",
                                   print_.upper()], env)
        if code != 0 or "Terms\n" not in out.replace("\r\n", "\n") \
                or f"Allowed: talker3 1.0.0, fingerprint {print_}" not in out:
            print(f"FAIL  --give: exit {code} {out!r} {err!r}")
            return 1
        if not os.path.exists(os.path.join(home, "consent.json")):
            print("FAIL  the default store isn't HAVNE_SETTINGS_DIR's "
                  "consent.json")
            return 1
        code, out, err = run(cli, ["plugins", "consent", "--list"], env)
        listed = [r for r in rows_of(out) if r.get("type") == "consent"]
        if code != 0 or len(listed) != 1 \
                or listed[0]["fingerprint"] != print_ \
                or listed[0]["id"] != "talker3":
            print(f"FAIL  --list: exit {code} {out!r}")
            return 1
        code, out, err = run(cli, ["plugins", "consent", "--withdraw",
                                   "talker3"], env)
        if code != 0 or not any(r.get("type") == "withdrawn"
                                for r in rows_of(out)):
            print(f"FAIL  --withdraw: exit {code} {out!r} {err!r}")
            return 1
        code, out, err = run(cli, ["plugins", "consent", "--list"], env)
        if code != 0 or any(r.get("type") == "consent"
                            for r in rows_of(out)):
            print(f"FAIL  --list after --withdraw: {out!r}")
            return 1
        bad = os.path.join(home, "bad.json")
        with open(bad, "w", encoding="utf-8") as f:
            f.write("not a store")
        code, out, err = run(cli, ["plugins", "consent", "--list",
                                   "--consent-store", bad], env)
        errors = [r for r in rows_of(out) if r.get("type") == "error"]
        if code != 3 or len(errors) != 1:
            print(f"FAIL  an unreadable store: exit {code} {out!r}")
            return 1
    print("ok    give, list and withdraw in the default store")
    return 0


def no_publisher_case(cli, package):
    code, out, err = run(cli, ["plugins", "terms", package])
    said = "this plugin names no publisher: it reads signals only"
    if code != 0 or out.strip() != said:
        print(f"FAIL  terms of a plugin with no publisher: exit {code} "
              f"{out!r}")
        return 1
    code, out, err = run(cli, ["plugins", "consent", package, "--give",
                               "0" * 64])
    if code != 2 or said not in err or out:
        print(f"FAIL  --give to a plugin with no publisher: exit {code} "
              f"{err!r}")
        return 1
    print("ok    a plugin with no publisher reads signals only")
    return 0


class Sim:
    """havne-cli sim's ELM327 on an OS-picked port, its later lines kept"""

    def __init__(self, cli):
        self.p = subprocess.Popen(
            [cli, "sim", "mock://sim?values=canned", "--elm327", "tcp:0",
             "--seconds", "60"],
            stdout=subprocess.PIPE, text=True)
        self.port = None
        for line in self.p.stdout:
            j = json.loads(line)
            if j["type"] == "elm327":
                self.port = int(j["listen"].rsplit(":", 1)[1])
                return
        raise SystemExit("FAIL  the emulator wrote no elm327 line")

    def clients(self):
        """Ends it; the connections it took"""
        self.p.terminate()
        rest, _ = self.p.communicate(timeout=60)
        return [json.loads(line) for line in rest.splitlines()
                if '"client"' in line and '"connected"' in line]


def stream(cli, port, package, store, env=None):
    uri = (f"elm327://127.0.0.1:{port}?profile=plugin.talker3.base"
           "&identify=0&diagnose=0&probe=0")
    return run(cli, ["stream", uri, "--plugin", package, "--consent-store",
                     store, "--count", "1"], env)


def changed_terms(package, folder):
    """package with one byte of its terms changed, its version kept"""
    with open(package, encoding="utf-8") as f:
        text = f.read()
    at = text.index("made up")
    path = os.path.join(folder, "talker3-changed.json")
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text[:at] + "Made up" + text[at + 7:])
    return path


def run_case(cli, package):
    print_ = fingerprint_of(cli, package)
    with tempfile.TemporaryDirectory(prefix="havne-cli-consent-run-") as d:
        store = os.path.join(d, "consent.json")
        bad = os.path.join(d, "bad.json")
        with open(bad, "w", encoding="utf-8") as f:
            f.write("not a store")
        sim = Sim(cli)
        code_bad, out_bad, err_bad = stream(cli, sim.port, package, bad)
        code, out, err = stream(cli, sim.port, package, store)
        took = sim.clients()
        if code_bad != 3 or out_bad or "consent store doesn't read" \
                not in err_bad:
            print(f"FAIL  an unreadable store: exit {code_bad} {out_bad!r} "
                  f"{err_bad!r}")
            return 1
        if code != 2 or out or took \
                or "this plugin needs your consent" not in err \
                or f"fingerprint {print_[:16]}\n" not in err \
                or print_ in err:
            print(f"FAIL  refused with no record: exit {code} {out!r} "
                  f"{err!r} {took!r}")
            return 1
        code, _, err = run(cli, ["plugins", "consent", package,
                                 "--consent-store", store, "--give", print_])
        if code != 0:
            print(f"FAIL  --give: exit {code} {err!r}")
            return 1
        sim = Sim(cli)
        try:
            code, out, err = stream(cli, sim.port, package, store)
        finally:
            sim.clients()
        session = [r for r in rows_of(out) if r.get("type") == "session"
                   and "ecu" not in r]
        if code != 0 or len(session) != 1 \
                or session[0].get("consent", {}).get("fingerprint") != print_:
            print(f"FAIL  a consented run's session line: exit {code} "
                  f"{session!r} {err!r}")
            return 1
        other = changed_terms(package, d)
        code, out, err = stream(cli, 1, other, store)
        if code != 2 or out or "The terms have changed." not in err \
                or "This file is not the same plugin" not in err:
            print(f"FAIL  a changed plugin's block: exit {code} {err!r}")
            return 1
        code, _, err = run(cli, ["plugins", "consent", "--consent-store",
                                 store, "--withdraw", "talker3"])
        code2, out, err2 = stream(cli, 1, package, store)
        if code != 0 or code2 != 2 or out \
                or "this plugin needs your consent" not in err2:
            print(f"FAIL  a withdrawn record: exit {code} {code2} {err2!r}")
            return 1
    print("ok    a run refused, consented, changed and withdrawn")
    return 0


def vcan_runs(cli, package, iface, d):
    """The vcan case's runs; None, or what failed"""
    print_ = fingerprint_of(cli, package)
    store = os.path.join(d, "consent.json")
    bad = os.path.join(d, "bad.json")
    with open(bad, "w", encoding="utf-8") as f:
        f.write("not a store")
    car = dict(os.environ, ECUIO_SOCKETCAN_TEST_VCAN_AS_CAN="1")
    uri = (f"socketcan://{iface}?profile=plugin.talker3.base"
           "&identify=0&diagnose=0")

    def stream(where, env=None):
        return run(cli, ["stream", uri, "--plugin", package,
                         "--consent-store", where, "--count", "1"], env)

    def session_of(out):
        return [r for r in rows_of(out) if r.get("type") == "session"
                and "ecu" not in r]

    code, out, err = stream(bad)
    shown = session_of(out)
    if code != 0 or len(shown) != 1 or "consent" in shown[0] \
            or "consent" in err:
        return f"vcan read a store: exit {code} {out!r} {err!r}"
    code, out, err = stream(store, car)
    if code != 2 or out or "this plugin needs your consent" not in err \
            or f"fingerprint {print_[:16]}\n" not in err:
        return f"a car's link without a record: exit {code} {out!r} {err!r}"
    code, out, err = run(cli, ["log", uri, os.path.join(d, "rec"),
                               "--plugin", package, "--consent-store", store,
                               "--seconds", "1"], car)
    if code != 2 or out or "this plugin needs your consent" not in err:
        return (f"log on a car's link without a record: exit {code} "
                f"{out!r} {err!r}")
    code, out, err = stream(bad, car)
    if code != 3 or out or "consent store doesn't read" not in err:
        return (f"a car's link, an unreadable store: exit {code} {out!r} "
                f"{err!r}")
    code, _, err = run(cli, ["plugins", "consent", package,
                             "--consent-store", store, "--give", print_])
    if code != 0:
        return f"--give: exit {code} {err!r}"
    code, out, err = stream(store, car)
    shown = session_of(out)
    if code != 0 or len(shown) != 1 \
            or shown[0].get("consent", {}).get("fingerprint") != print_:
        return f"a car's link with a record: exit {code} {shown!r} {err!r}"
    return None


def vcan_case(cli, package):
    """vcan binds unasked and reads no store; judged a car's link
    (ECUIO_SOCKETCAN_TEST_VCAN_AS_CAN), it refuses as ELM327 does until a
    record is given"""
    iface = os.environ.get("ECUIO_VCAN_IF", "vcan0")
    if not os.path.exists(f"/sys/class/net/{iface}"):
        if os.environ.get("ECUIO_VCAN_REQUIRED") == "1":
            print(f"FAIL  no {iface}")
            return 1
        print(f"skipped: no {iface}")
        return 77
    sim = subprocess.Popen([cli, "sim", "mock://sim?values=canned", "--on",
                            iface, "--seconds", "60"],
                           stdout=subprocess.PIPE, text=True)
    try:
        for line in sim.stdout:
            if json.loads(line)["type"] in ("sim", "error"):
                break
        with tempfile.TemporaryDirectory(prefix="havne-cli-consent-vcan-") as d:
            failed = vcan_runs(cli, package, iface, d)
    finally:
        sim.terminate()
        sim.wait(timeout=10)
    if failed is not None:
        print(f"FAIL  {failed}")
        return 1
    print("ok    vcan reads no store; a car's link refuses, then runs consented")
    return 0


def unlock_stream(cli, package, consent, security, folder):
    """stream --unlock pcm:01 for 8 s on an emulator of its own, the
    exchanges logged in a temporary folder of its own: (exit, rows,
    standard error, the wire logs' text, the emulator's connections)"""
    logs = tempfile.mkdtemp(prefix="wire-", dir=folder)
    env = dict(os.environ, TMP=logs, TEMP=logs, TMPDIR=logs,
               ECUIO_SUR_LOG_FRAMES="1")
    sim = Sim(cli)
    uri = (f"elm327://127.0.0.1:{sim.port}?profile=plugin.talker.base"
           "&diagnose=0&probe=0")
    try:
        code, out, err = run(cli, ["stream", uri, "--plugin", package,
                                   "--consent-store", consent,
                                   "--unlock", "pcm:01",
                                   "--security-store", security,
                                   "--vehicle", "bench", "--seconds", "8"],
                             env)
    finally:
        took = sim.clients()
    wire = ""
    for name in sorted(os.listdir(logs)):
        if name.startswith("ecuio-elm327"):
            with open(os.path.join(logs, name), encoding="utf-8",
                      errors="replace") as f:
                wire += f.read()
    return code, rows_of(out), err, wire, took


# How the ELM327 wire log shows level 01's seed request typed
SEED_SENT = "] > 2701"


def seeds_asked(wire):
    """The seed requests (27 01) the wire logs show typed"""
    return sum(1 for line in wire.splitlines() if SEED_SENT in line)


def of_type(rows, kind):
    return [r for r in rows if r.get("type") == kind]


def unlock_case(cli, writer):
    with tempfile.TemporaryDirectory(prefix="havne-cli-unlock-") as d:
        right = os.path.join(d, "right.json")
        wrong = os.path.join(d, "wrong.json")
        done = subprocess.run([writer, right, wrong], capture_output=True)
        if done.returncode != 0:
            print(f"FAIL  the fixture writer: {done.stderr!r}")
            return 1
        for package, name in ((right, "right"), (wrong, "wrong")):
            code, _, err = run(cli, ["plugins", "consent", package,
                                     "--consent-store",
                                     os.path.join(d, name + "-consent.json"),
                                     "--give", fingerprint_of(cli, package)])
            if code != 0:
                print(f"FAIL  --give {name}: exit {code} {err!r}")
                return 1
        code, rows, err, wire, _ = unlock_stream(
            cli, right, os.path.join(d, "right-consent.json"),
            os.path.join(d, "right-security.json"), d)
        words = of_type(rows, "unlock")
        talk = of_type(rows, "conversation")
        if code != 0 or [w.get("outcome") for w in words] != ["asked"]                 or not any(t.get("unlock") == "unlocked" for t in talk)                 or seeds_asked(wire) != 1:
            print(f"FAIL  the right key: exit {code} {words!r} {talk!r} "
                  f"{seeds_asked(wire)} seeds {err!r}")
            return 1
        consent = os.path.join(d, "wrong-consent.json")
        security = os.path.join(d, "wrong-security.json")
        code, rows, err, wire, _ = unlock_stream(cli, wrong, consent,
                                                 security, d)
        words = of_type(rows, "unlock")
        talk = of_type(rows, "conversation")
        if code != 0 or [w.get("outcome") for w in words] != ["asked"]                 or not any(t.get("unlock") == "failed"
                           and t.get("failed") == 1 for t in talk)                 or any(t.get("unlock") == "unlocked" for t in talk)                 or seeds_asked(wire) != 1:
            print(f"FAIL  the wrong key: exit {code} {words!r} {talk!r} "
                  f"{seeds_asked(wire)} seeds {err!r}")
            return 1
        code, rows, err, wire, took = unlock_stream(cli, wrong, consent,
                                                    security, d)
        words = of_type(rows, "unlock")
        if code != 0 or len(words) != 1                 or words[0].get("outcome") != "delayed"                 or not words[0].get("wait_ms") or not took                 or seeds_asked(wire) != 0:
            print(f"FAIL  inside the delay: exit {code} {words!r} "
                  f"{seeds_asked(wire)} seeds {len(took)} connections "
                  f"{err!r}")
            return 1
    print("ok    unlocked once; a wrong key failed, then delayed unsent")
    return 0


def main(argv):
    if len(argv) == 4 and argv[1] == "unlock":
        return unlock_case(argv[2], argv[3])
    if len(argv) == 4 and argv[1] == "run":
        return run_case(argv[2], argv[3])
    if len(argv) == 4 and argv[1] == "vcan":
        return vcan_case(argv[2], argv[3])
    if len(argv) >= 5 and argv[1] == "terms":
        return terms_case(argv[2], argv[3], argv[4], "--update" in argv[5:])
    if len(argv) == 4 and argv[1] == "store":
        return store_case(argv[2], argv[3])
    if len(argv) == 4 and argv[1] == "no-publisher":
        return no_publisher_case(argv[2], argv[3])
    print(__doc__ or "usage: cli_consent.py terms|store|no-publisher ...")
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
