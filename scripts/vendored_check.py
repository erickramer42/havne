# File/layer: scripts/vendored_check.py | Python 3.8+, no packages
# Testing: UNIT, its own self-test (ctest VendoredTest.SelfTest); ctest
#          VendoredTest.WamrIsItsListedBytes runs check
#
# A vendored folder against its committed list of hashes, with no network:
# third_party/wamr against third_party/wamr.sha256. scripts/vendor_wamr.sh
# writes both from upstream's tag and its --check compares the folder with
# the tag; this says, in every build, that nobody has changed a file there
# since. A deliberate change is a patch in third_party/wamr-patches, which
# the script applies before it writes the list.
#
#   python3 scripts/vendored_check.py check [<folder> <list>]
#   python3 scripts/vendored_check.py write [<folder> <list>]
#   python3 scripts/vendored_check.py selftest
#
# The list is one line per file, "<sha256>  <path>", sorted by path, the
# paths relative to the folder with forward slashes: sha256sum -c reads it
# from inside the folder.

import hashlib
import os
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FOLDER = os.path.join(ROOT, "third_party", "wamr")
LIST = os.path.join(ROOT, "third_party", "wamr.sha256")
SHOWN = 20  # paths named for each kind of difference


def hashes_of(folder):
    """Each file under folder by its path from it: its bytes' SHA-256"""
    found = {}
    for base, _, names in os.walk(folder):
        for name in names:
            full = os.path.join(base, name)
            rel = os.path.relpath(full, folder).replace(os.sep, "/")
            with open(full, "rb") as f:
                found[rel] = hashlib.sha256(f.read()).hexdigest()
    return found


def read_list(path):
    """The list's lines as path -> hash; raises ValueError on a bad line"""
    listed = {}
    with open(path, "r", encoding="utf-8", newline="") as f:
        for n, line in enumerate(f.read().split("\n"), 1):
            line = line.rstrip("\r")  # a Windows checkout of the list
            if line == "":
                continue
            digest, sep, rel = line.partition("  ")
            if (sep == "" or len(digest) != 64 or rel == "" or rel in listed
                    or any(c not in "0123456789abcdef" for c in digest)):
                raise ValueError("line %d isn't '<sha256>  <path>', once" % n)
            listed[rel] = digest
    return listed


def write_list(folder, path):
    found = hashes_of(folder)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        for rel in sorted(found):
            f.write("%s  %s\n" % (found[rel], rel))
    return len(found)


def differences(folder, path):
    """What differs, as lines of words; empty when the folder is the list's"""
    try:
        listed = read_list(path)
    except (OSError, ValueError) as e:
        return ["the list can't be read: %s" % e]
    found = hashes_of(folder)
    kinds = (
        ("changed", sorted(r for r in found if r in listed and found[r] != listed[r])),
        ("not in the list", sorted(r for r in found if r not in listed)),
        ("listed but missing", sorted(r for r in listed if r not in found)),
    )
    lines = []
    for what, paths in kinds:
        for rel in paths[:SHOWN]:
            lines.append("%s: %s" % (what, rel))
        if len(paths) > SHOWN:
            lines.append("%s: and %d more" % (what, len(paths) - SHOWN))
    if not listed and not lines:
        lines.append("the list is empty")
    return lines


def check(folder, path):
    lines = differences(folder, path)
    if lines:
        for line in lines:
            print(line)
        print("FAIL  %s is not what %s lists. A change there is a patch in "
              "third_party/wamr-patches, applied by scripts/vendor_wamr.sh"
              % (os.path.basename(folder), os.path.basename(path)))
        return 1
    print("ok    %s is what %s lists" % (os.path.basename(folder), os.path.basename(path)))
    return 0


def selftest():
    def put(base, rel, data):
        full = os.path.join(base, *rel.split("/"))
        os.makedirs(os.path.dirname(full), exist_ok=True)
        with open(full, "wb") as f:
            f.write(data)

    with tempfile.TemporaryDirectory() as work:
        folder = os.path.join(work, "lib")
        listing = os.path.join(work, "lib.sha256")
        put(folder, "LICENSE", b"words\n")
        put(folder, "core/a.c", b"int a;\n")
        put(folder, "core/deep/b.h", b"")
        assert write_list(folder, listing) == 3
        text = open(listing, "rb").read().decode("utf-8")
        assert "\r" not in text and text.endswith("\n")
        assert [line.split("  ")[1] for line in text.splitlines()] == [
            "LICENSE", "core/a.c", "core/deep/b.h"], text
        assert text.splitlines()[2].startswith(hashlib.sha256(b"").hexdigest())
        assert differences(folder, listing) == []
        with open(listing, "wb") as f:
            f.write(text.replace("\n", "\r\n").encode("utf-8"))
        assert differences(folder, listing) == [], "the list itself may come as CRLF"

        put(folder, "core/a.c", b"int a;\r\n")  # a checkout that changed line ends
        assert differences(folder, listing) == ["changed: core/a.c"]
        put(folder, "core/a.c", b"int a;\n")
        put(folder, "core/new.c", b"")
        assert differences(folder, listing) == ["not in the list: core/new.c"]
        os.remove(os.path.join(folder, "core", "new.c"))
        os.remove(os.path.join(folder, "LICENSE"))
        assert differences(folder, listing) == ["listed but missing: LICENSE"]
        put(folder, "LICENSE", b"words\n")
        assert differences(folder, listing) == []

        for n in range(SHOWN + 5):
            put(folder, "more/%02d.c" % n, b"x")
        many = differences(folder, listing)
        assert len(many) == SHOWN + 1 and many[-1] == "not in the list: and 5 more", many

        for bad in ("zz  a\n", "%s a\n" % ("0" * 64), "%s  \n" % ("0" * 64),
                    "%s  a\n%s  a\n" % ("0" * 64, "1" * 64), "%s  a\n" % ("0" * 63)):
            with open(listing, "w", encoding="utf-8", newline="\n") as f:
                f.write(bad)
            got = differences(folder, listing)
            assert len(got) == 1 and got[0].startswith("the list can't be read"), (bad, got)
        with open(listing, "w", encoding="utf-8", newline="\n") as f:
            f.write("")
        empty = os.path.join(work, "empty")
        os.makedirs(empty)
        assert differences(empty, listing) == ["the list is empty"]
        assert differences(folder, os.path.join(work, "none.sha256"))[0].startswith(
            "the list can't be read")
    print("vendored_check selftest: ok")
    return 0


def main(argv):
    if len(argv) == 2 and argv[1] == "selftest":
        return selftest()
    if len(argv) in (2, 4) and argv[1] in ("check", "write"):
        folder, path = (argv[2], argv[3]) if len(argv) == 4 else (FOLDER, LIST)
        if argv[1] == "check":
            return check(folder, path)
        print("ok    %s: %d files listed" % (os.path.basename(path), write_list(folder, path)))
        return 0
    print("usage: vendored_check.py check|write [<folder> <list>] | selftest", file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
