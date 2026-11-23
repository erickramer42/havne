# File/layer: tests/cli_awake.py | test-only, Python 3.8+, no packages
# Testing: HARNESS (ctest CliLogRunTest.ALiveSessionLineSaysAwake): the
#          host kept awake from outside the process (ADR-0033's appendix of
#          2026-11-22)
#
#   python tests/cli_awake.py <havne-cli>
#
# Over the emulator's ELM327 (cli_log.py's Emulator): a live log's session
# line says awake, true or (where the host can't hold) false beside one
# standard-error line; false with --allow-sleep and no such line; the
# mock's session line has no awake key.

import sys
import tempfile

sys.dont_write_bytecode = True  # no __pycache__ beside the tests
from cli_log import Emulator, run  # noqa: E402

REFUSED = "havne-cli: couldn't keep the host awake: "


def opening_session(rows):
    return next((r for r in rows if r.get("type") == "session"
                 and "ecu" not in r), None)


def live_cases(cli, elm):
    with tempfile.TemporaryDirectory(prefix="havne-cli-awake-") as root:
        code, rows, err = run(cli, ["log", elm, root, "--signal", "RPM",
                                    "--seconds", "1"])
        line = opening_session(rows)
        if code != 0 or line is None or "awake" not in line:
            print(f"FAIL  a live log's session line: exit {code} {line!r}")
            return 1
        if line["awake"] is not True and REFUSED not in err:
            print(f"FAIL  awake false with no refusal said: {err!r}")
            return 1
        if line["awake"] is True and REFUSED in err:
            print(f"FAIL  held and said refused: {err!r}")
            return 1
        code, rows, err = run(cli, ["log", elm, root, "--signal", "RPM",
                                    "--seconds", "1", "--allow-sleep"])
        line = opening_session(rows)
        if code != 0 or line is None or line.get("awake") is not False \
                or REFUSED in err:
            print(f"FAIL  --allow-sleep: exit {code} {line!r} {err!r}")
            return 1
    return 0


def mock_case(cli):
    code, rows, err = run(cli, ["stream", "mock://", "--count", "1"])
    line = opening_session(rows)
    if code != 0 or line is None or "awake" in line or REFUSED in err:
        print(f"FAIL  the mock's session line: exit {code} {line!r}")
        return 1
    return 0


def main(argv):
    if len(argv) < 2:
        print("usage: cli_awake.py <havne-cli>")
        return 2
    cli = argv[1]
    emulator = Emulator(cli)
    try:
        failed = live_cases(cli, f"elm327://127.0.0.1:{emulator.port}")
    finally:
        emulator.end()
    failed = failed or mock_case(cli)
    if not failed:
        print("ok    a live session line says awake, --allow-sleep false, "
              "the mock none")
    return failed


if __name__ == "__main__":
    sys.exit(main(sys.argv))
