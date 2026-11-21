#!/usr/bin/env python3
# File/layer: tests/plaintext_pin.py | Python, source pin for app/qml
# Testing: ctest-only: PlainTextPinTest.EveryTextSetsItsFormat scans app/qml,
#          PlainTextPinTest.SelfTest runs the scanner on made-up files
#
# Every Text and Label in app/qml sets textFormat. A Text's image tag
# (<img src="file://host/a">) is loaded without the engine's URL
# interceptor or network manager being asked, so a text that draws a
# string from outside (a file, an adapter's reply, core's error detail)
# must be plain. An element that draws markup on purpose is in ALLOWED,
# by file and objectName, with its reason.
#
#   plaintext_pin.py <source root>      scan <root>/app/qml; exit 1 on a finding
#   plaintext_pin.py --selftest

import re
import sys
from pathlib import Path

# (file, objectName): why its format may not be plain
ALLOWED = {
    ("FrameIdList.qml", "idPayload"):
        "StyledText on purpose: markedPayload wraps hex digits and spaces in <font><b>",
}

ELEMENT = re.compile(r"(?<![\w.])(Label|Text)\s*\{")


def blank_strings_and_comments(src):
    """src with string bodies and comments replaced by spaces, same length."""
    out = []
    i = 0
    n = len(src)
    while i < n:
        c = src[i]
        if src.startswith("//", i):
            j = src.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif src.startswith("/*", i):
            j = src.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append(re.sub(r"[^\n]", " ", src[i:j]))
            i = j
        elif c in "\"'`":
            j = i + 1
            while j < n and src[j] != c:
                j += 2 if src[j] == "\\" else 1
            j = min(j + 1, n)
            out.append(c + re.sub(r"[^\n]", " ", src[i + 1:j - 1]) + c)
            i = j
        else:
            out.append(c)
            i += 1
    return "".join(out)


def body_of(clean, open_at):
    """Index just past the brace that closes the one at open_at."""
    depth = 0
    for j in range(open_at, len(clean)):
        if clean[j] == "{":
            depth += 1
        elif clean[j] == "}":
            depth -= 1
            if depth == 0:
                return j + 1
    return len(clean)


def direct_text(clean, start, end):
    """The element's own body: nested { } blocks removed."""
    out = []
    depth = 0
    for j in range(start, end):
        c = clean[j]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
        elif depth == 1:
            out.append(c)
    return "".join(out)


def scan_text(name, src):
    clean = blank_strings_and_comments(src)
    findings = []
    for m in ELEMENT.finditer(clean):
        open_at = m.end() - 1
        end = body_of(clean, open_at)
        own = direct_text(clean, open_at, end)
        if re.search(r"(?<![\w.])textFormat\s*:", own):
            continue
        obj = re.search(r"objectName\s*:\s*\"([^\"]*)\"", src[open_at:end])
        key = (name, obj.group(1) if obj else None)
        if key in ALLOWED:
            continue
        line = src.count("\n", 0, m.start()) + 1
        findings.append("%s:%d: %s sets no textFormat" % (name, line, m.group(1)))
    return findings


def scan_root(root):
    findings = []
    files = sorted((Path(root) / "app" / "qml").glob("*.qml"))
    if not files:
        return ["no QML files under %s/app/qml" % root]
    for f in files:
        findings += scan_text(f.name, f.read_text(encoding="utf-8"))
    return findings


def selftest():
    bad = 'Item {\n  Label { text: x }\n  Text { text: "}" }\n}\n'
    assert len(scan_text("a.qml", bad)) == 2, scan_text("a.qml", bad)
    good = ('Label { textFormat: Text.PlainText; text: x }\n'
            'Text {\n  textFormat: Text.PlainText\n  Label { textFormat: Text.PlainText }\n}\n')
    assert scan_text("a.qml", good) == [], scan_text("a.qml", good)
    # A child's format is not the parent's
    nested = 'Label {\n  Text { textFormat: Text.PlainText }\n}\n'
    assert len(scan_text("a.qml", nested)) == 1
    # Strings and comments hold no elements or formats
    tricky = 'Label {\n  text: "Text { textFormat: x }" // Label {\n  textFormat: Text.PlainText\n}\n'
    assert scan_text("a.qml", tricky) == []
    inline = 'component Tag: Label { color: "red" }\n'
    assert len(scan_text("a.qml", inline)) == 1
    # PlainLabel, Text.PlainText and the like are not elements
    assert scan_text("a.qml", "PlainLabel { }\nx: Text.Wrap\n") == []
    allowed = 'Text { objectName: "idPayload"; textFormat: Text.StyledText }\n'
    assert scan_text("FrameIdList.qml", 'Text { objectName: "idPayload" }\n') == []
    assert len(scan_text("Other.qml", 'Text { objectName: "idPayload" }\n')) == 1
    assert allowed
    print("ok")


def main(argv):
    if len(argv) == 2 and argv[1] == "--selftest":
        selftest()
        return 0
    if len(argv) != 2:
        print(__doc__)
        return 2
    findings = scan_root(argv[1])
    for f in findings:
        print(f)
    return 1 if findings else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
