# File/layer: tests/wheel_installed.py | Python 3.11+, the havne wheel
# Testing: HARNESS, WheelTest.Installed (tests/CMakeLists.txt; ADR-0053
#          section 7, "the wheel itself")
#
# The wheel as a user gets it: packaging/python/make_wheel.py over this
# tree's shared core (its platform tag the library's own), built twice to
# the same bytes, then packaging/python/check_wheel.py, release.yml's own
# check: installed into a fresh venv and used from outside the tree with
# HAVNE_LIBRARY unset, read_pids.py through it, and on Windows the fake
# DLL through the wheel's x64 surrogate, and the x86 fake through its x86
# one where the tree has them (CI's win-build builds no Win32 tree).
# Exit 77 (a skip) where this Python can't make a venv with pip, unless
# ECUIO_PYTEST_REQUIRED=1.
#
#   python tests/wheel_installed.py --library <ecuio> [--surrogate-x86 <exe>
#       --surrogate-x64 <exe> --fake-dll <fake_j2534.dll>
#       --fake-dll-x86 <the Win32 tree's fake_j2534.dll>]

import argparse
import filecmp
import os
import sys
import tempfile

sys.dont_write_bytecode = True
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "packaging", "python"))

import check_wheel  # noqa: E402
import make_wheel  # noqa: E402


def main(argv):
    p = argparse.ArgumentParser()
    p.add_argument("--library", required=True)
    p.add_argument("--surrogate-x86")
    p.add_argument("--surrogate-x64")
    p.add_argument("--fake-dll")
    p.add_argument("--fake-dll-x86")
    args = p.parse_args(argv)
    version = make_wheel.package_version(os.path.join(ROOT, "python", "havne"))
    surrogates = {k: v for k, v in (("x86", args.surrogate_x86), ("x64", args.surrogate_x64)) if v}

    with tempfile.TemporaryDirectory(prefix="havne-wheel-") as tmp:
        wheels = [make_wheel.build(version, args.library, os.path.join(tmp, d),
                                   surrogates=surrogates or None, epoch=1767225600,
                                   missing_surrogates_ok=True)
                  for d in ("a", "b")]
        if not filecmp.cmp(wheels[0], wheels[1], shallow=False):
            print("FAIL  two builds of the wheel differ")
            return 1
        print("built", os.path.basename(wheels[0]), "twice, the same bytes")
        fakes = [args.fake_dll] if args.fake_dll else []
        if args.fake_dll_x86 and args.surrogate_x86:
            fakes.append(args.fake_dll_x86)
        return check_wheel.check(wheels[0], fakes)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
