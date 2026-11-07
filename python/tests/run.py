# File/layer: python/tests/run.py | Python 3.11+, the havne package's tests
# Testing: runs them: each ctest PythonTest.<file> (tests/CMakeLists.txt)
#
# One test file under pytest for ctest: exit 77 (ctest's skip) when pytest
# isn't installed or every case in the file skipped (no vcan0, not
# Windows), so a skip shows as one; else pytest's own exit status. Under
# ECUIO_PYTEST_REQUIRED=1 (CI's jobs that build the shared core) a
# missing pytest fails instead. No cache or bytecode is written into the
# tree.
#
#   python -B python/tests/run.py python/tests/test_session.py

import os
import sys

sys.dont_write_bytecode = True


def main(argv):
    if len(argv) != 2:
        print("usage: run.py <test file>", file=sys.stderr)
        return 2
    try:
        import pytest
    except ImportError:
        if os.environ.get("ECUIO_PYTEST_REQUIRED") == "1":
            print("FAIL  pytest isn't installed for this Python, and ECUIO_PYTEST_REQUIRED is set")
            return 1
        print("skipped: pytest isn't installed for this Python")
        return 77

    class Outcomes:
        passed = skipped = failed = 0

        def pytest_collectreport(self, report):
            if report.skipped:   # a module-level skip: no vcan0, not Windows
                self.skipped += 1

        def pytest_runtest_logreport(self, report):
            if report.skipped:
                self.skipped += 1
            elif report.failed:
                self.failed += 1
            elif report.when == "call":
                self.passed += 1

    seen = Outcomes()
    rc = pytest.main(["-q", "-p", "no:cacheprovider", "-W", "error::pytest.PytestUnraisableExceptionWarning",
                      argv[1]], plugins=[seen])
    # 5: no tests collected, as when the module skipped itself
    if rc in (0, 5) and seen.passed == 0 and seen.failed == 0 and seen.skipped > 0:
        return 77
    return int(rc)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
