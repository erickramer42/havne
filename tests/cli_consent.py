# File/layer: tests/cli_consent.py | test-only, Python 3.8+, no packages
# Testing: HARNESS (ctest CliConsentTest.*): havne-cli's consent words from
#          outside the process (ADR-0066's appendix of 2026-11-23)
#
#   python tests/cli_consent.py terms <havne-cli> <talker3.json> <golden.txt>
#                                     [--update]
#   python tests/cli_consent.py store <havne-cli> <talker3.json>
#   python tests/cli_consent.py no-publisher <havne-cli> <format-2 package>
#
# terms: `plugins terms`' text against its golden, byte for byte (line
# ends aside), the package's path written as <package>. store: give, list
# and withdraw in a scratch HAVNE_SETTINGS_DIR, whose consent.json is the
# default store; an unreadable store is exit 3 with an error line.
# no-publisher: a plugin with no publisher says it reads signals only, and
# takes no consent.

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


def main(argv):
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
