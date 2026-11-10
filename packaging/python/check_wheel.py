# File/layer: packaging/python/check_wheel.py | Python 3.11+, the havne wheel
# Testing: HARNESS, WheelTest.Installed (tests/wheel_installed.py calls
#          check()); release.yml runs it on each wheel it builds
#
# A built wheel as a user gets it: installed into a fresh virtual
# environment with pip --no-index, then used from a folder outside the
# tree with HAVNE_LIBRARY, PYTHONPATH and the surrogates' variables unset.
# The package must find its own library in site-packages (the single
# libecuio.so.<soname> file on Linux), on Windows name its own surrogates
# where the wheel has them, and python/examples/read_pids.py must read
# mock:// through it. Each fake J2534 DLL given then opens through the
# wheel's surrogate of its own architecture. An imported log is exported
# (pcapng) and reported on through it too.
#
#   python packaging/python/check_wheel.py <wheel> [--fake-dll <dll>]...
#
# Exit 0 when it works, 1 when it doesn't, 77 (a skip) when this Python
# makes no venv with pip, unless ECUIO_PYTEST_REQUIRED=1 makes that a
# failure. release.yml sets it, so a release never skips.

import argparse
import os
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

CHECK = r'''
import os, sys, sysconfig, time
import havne
package = os.path.join(sysconfig.get_paths()["platlib"], "havne")
assert os.path.samefile(os.path.dirname(havne.__file__), package), havne.__file__
assert os.path.samefile(os.path.dirname(havne.library_path), package), havne.library_path
print("library", os.path.basename(havne.library_path), "core", havne.core_version)
if sys.platform == "win32":
    for var, name in (("ECUIO_SURROGATE_EXE", "ecuio-j2534-surrogate.exe"),
                      ("ECUIO_SURROGATE_X64_EXE", "ecuio-j2534-surrogate-x64.exe")):
        if os.path.isfile(os.path.join(package, name)):   # a tree with no x86 one: CI's
            assert os.path.samefile(os.environ[var], os.path.join(package, name)), var
# The export and health modules are in the wheel too: a pcapng and a report of an imported log
import pathlib, tempfile
with tempfile.TemporaryDirectory() as t:
    t = pathlib.Path(t)
    (t / "bus.log").write_text("(1700000000.250000) can0 7E8#04410C1AF8\n", newline="\n")
    havne.import_candump(t / "bus.log", t / "rec")
    assert havne.export_pcapng(t / "rec", t / "bus.pcapng").frames == 1
    assert havne.health_report(t / "rec", t / "health.txt", "text").source == "recording"
print("export and health report through the wheel")
for fake in sys.argv[1:]:
    with havne.Session("j2534://" + fake.replace("\\", "/")) as s:
        assert s.info.backend == havne.c.ECUIO_BACKEND_J2534_SURROGATE
        values = s.values()
        s.start()
        until, rpm = time.monotonic() + 15, []
        while not rpm and time.monotonic() < until:
            rpm = [v for v in values.read(0.2) if v.name == "RPM"]
        assert rpm, f"{fake}'s ECU answered through the wheel's surrogate"
    print("j2534 through the wheel's surrogate:", fake)
print("ok")
'''


def skip_or_fail(why):
    if os.environ.get("ECUIO_PYTEST_REQUIRED") == "1":
        print(f"FAIL  {why}, and ECUIO_PYTEST_REQUIRED is set")
        return 1
    print(f"skipped: {why}")
    return 77


def run(cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", timeout=300, **kw)
    if r.stdout.strip():
        print(r.stdout.rstrip())
    if r.returncode != 0:
        print(r.stderr.rstrip())
    return r.returncode


def check(wheel, fakes=()):
    """0 when the installed wheel works, 1 when it doesn't, 77 for a skip"""
    with tempfile.TemporaryDirectory(prefix="havne-check-") as tmp:
        venv = os.path.join(tmp, "venv")
        made = subprocess.run([sys.executable, "-m", "venv", venv], capture_output=True, text=True)
        if made.returncode != 0:
            return skip_or_fail(f"this Python makes no venv ({made.stderr.strip()[-200:]})")
        py = os.path.join(venv, "Scripts" if sys.platform == "win32" else "bin",
                          "python.exe" if sys.platform == "win32" else "python")
        env = {k: v for k, v in os.environ.items()
               if k not in ("HAVNE_LIBRARY", "PYTHONPATH", "PYTHONHOME",
                            "ECUIO_SURROGATE_EXE", "ECUIO_SURROGATE_X64_EXE")}
        env.update(PYTHONDONTWRITEBYTECODE="1", PYTHONIOENCODING="utf-8")
        if run([py, "-m", "pip", "--version"], env=env) != 0:
            return skip_or_fail("the venv has no pip")
        if run([py, "-m", "pip", "install", "-q", "--no-index", "--no-deps",
                "--disable-pip-version-check", os.path.abspath(wheel)], env=env, cwd=tmp) != 0:
            print("FAIL  pip refused the wheel")
            return 1
        script = os.path.join(tmp, "check.py")
        with open(script, "w", encoding="utf-8") as f:
            f.write(CHECK)
        if run([py, "-B", script] + list(fakes), env=env, cwd=tmp) != 0:
            print("FAIL  the installed package")
            return 1
        example = os.path.join(ROOT, "python", "examples", "read_pids.py")
        if run([py, "-B", example, "mock://wheel"], env=env, cwd=tmp) != 0:
            print("FAIL  read_pids.py through the installed wheel")
            return 1
    print("ok")
    return 0


def main(argv):
    p = argparse.ArgumentParser()
    p.add_argument("wheel")
    p.add_argument("--fake-dll", action="append", default=[])
    args = p.parse_args(argv)
    return check(args.wheel, args.fake_dll)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
