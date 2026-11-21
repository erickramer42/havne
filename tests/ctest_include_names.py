# File/layer: tests/ctest_include_names.py | ctest-only script
# Testing: CtestIncludeNamesTest.TheJ2534TimeoutNamesExist
"""A CTest include's set_tests_properties ignores a name that no longer
exists, so each name in j2534_timeouts.cmake's list must still be a
TEST_F(J2534ConnectTest, <name>) in tests/test_j2534_connect*.cpp."""
import pathlib
import re
import sys

here = pathlib.Path(__file__).resolve().parent
text = (here / "j2534_timeouts.cmake").read_text(encoding="utf-8")
block = re.search(r"foreach\(_name\s+(.*?)\)\s*set_tests_properties", text, re.S)
names = block.group(1).split() if block else []
if not names:
    print("no names found in j2534_timeouts.cmake")
    sys.exit(1)
sources = "".join(p.read_text(encoding="utf-8", errors="replace")
                  for p in here.glob("test_j2534_connect*.cpp"))
missing = [n for n in names
           if not re.search(r"TEST_F\(\s*J2534ConnectTest\s*,\s*" + re.escape(n) + r"\s*\)",
                            sources)]
if missing:
    print("not a J2534ConnectTest case any more: " + ", ".join(missing))
    sys.exit(1)
print("ok")
