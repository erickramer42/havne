#!/usr/bin/env python3
# File/layer: tests/plaintext_pin.py | Python, source pin for app/qml
# Testing: ctest-only: PlainTextPinTest.EveryTextSetsItsFormat scans app/qml,
#          PlainTextPinTest.SelfTest runs the scanner on made-up files
#
# Every Text and Label in app/qml sets textFormat: Text.PlainText. A Text's
# image tag (<img src="file://host/a">) is loaded without the engine's URL
# interceptor or network manager being asked, so a text that draws a
# string from outside (a file, an adapter's reply, core's error detail)
# must be plain. An element that draws markup on purpose is in ALLOWED,
# by file and objectName, with the one format it may use. The stock
# controls draw AutoText: each needs a contentItem of its own, or a text,
# title or model that is a string literal (qsTr) alone; a TextEdit or
# TextArea keeps its plain default or says so.
#
# Strings end at a newline and an unterminated one is a finding, so a
# regex literal holding a quote can't hide the rest of a file.
#
#   plaintext_pin.py <source root>      scan <root>/app/qml; exit 1 on a finding
#   plaintext_pin.py --selftest

import re
import sys
from pathlib import Path

# (file, objectName): the format it may use instead of Text.PlainText.
# idPayload: markedPayload wraps hex digits and spaces in <font><b>
ALLOWED = {("FrameIdList.qml", "idPayload"): "Text.StyledText"}

ELEMENT = re.compile(r"(?<![\w.])(Label|Text)\s*\{")
STOCK_NAMES = ("Switch|CheckBox|RadioButton|Button|ItemDelegate|MenuItem|ComboBox|"
               "Dialog|ToolTip|TabButton|ToolButton|RoundButton|CheckDelegate|"
               "SwitchDelegate|RadioDelegate|SwipeDelegate|GroupBox")
STOCK = re.compile(r"(?<![\w.])(%s)\s*\{" % STOCK_NAMES)
EDIT = re.compile(r"(?<![\w.])(TextEdit|TextArea)\s*\{")
# a string body is blanked before a value is matched
LITERAL = r'(?:qsTr\(\s*"[ ]*"\s*\)|"[ ]*")'


def blank_strings_and_comments(src, problems=None):
    """src with string bodies and comments replaced by spaces, same length.

    A " or ' string ends at a newline (an unterminated one adds its line
    to problems); a backtick string may span lines."""
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
            while j < n and src[j] != c and (c == "`" or src[j] != "\n"):
                j += 2 if src[j] == "\\" else 1
            j = min(j, n)
            closed = j < n and src[j] == c
            if not closed and problems is not None:
                problems.append(src.count("\n", 0, i) + 1)
            out.append(c + re.sub(r"[^\n]", " ", src[i + 1:j]) + (c if closed else ""))
            i = j + 1 if closed else j
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


def direct_positions(clean, start, end):
    """(the element's own body with nested { } blocks removed, the clean
    index of each of its characters)."""
    out = []
    at = []
    depth = 0
    for j in range(start, end):
        c = clean[j]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
        elif depth == 1:
            out.append(c)
            at.append(j)
    return "".join(out), at


def value_of(clean, own, at, key):
    """The expression after `key:` among own's characters (at maps each to
    its index in clean), read from clean up to a newline or ; outside
    brackets; None when own has no such property."""
    m = re.search(r"(?<![\w.])%s\s*:" % re.escape(key), own)
    if not m:
        return None
    depth = 0
    start = j = at[m.end() - 1] + 1
    while j < len(clean):
        c = clean[j]
        if c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
            if depth < 0:
                break
        elif depth == 0 and c in "\n;":
            break
        j += 1
    return " ".join(clean[start:j].split())


def is_literal(value):
    """A string literal, qsTr(...) of one, or an array of them."""
    if re.fullmatch(LITERAL, value):
        return True
    return re.fullmatch(r"\[\s*(?:%s\s*,?\s*)*\]" % LITERAL, value) is not None


def has_prop(own, key):
    return re.search(r"(?<![\w.])%s\s*:" % key, own) is not None


def scan_text(name, src):
    problems = []
    clean = blank_strings_and_comments(src, problems)
    findings = ["%s:%d: unterminated string" % (name, ln) for ln in problems]

    def line(pos):
        return src.count("\n", 0, pos) + 1

    for m in ELEMENT.finditer(clean):
        open_at = m.end() - 1
        own, at = direct_positions(clean, open_at, body_of(clean, open_at))
        fmt = value_of(clean, own, at, "textFormat")
        obj = re.search(r"(?<![\w.])objectName\s*:\s*\"", own)
        obj_name = None
        if obj:
            q = at[obj.end() - 1]
            obj_name = src[q + 1:clean.index('"', q + 1)]
        want = ALLOWED.get((name, obj_name), "Text.PlainText")
        if fmt == want:
            continue
        why = "sets no textFormat" if fmt is None else "sets textFormat: %s, not %s" % (fmt, want)
        findings.append("%s:%d: %s %s" % (name, line(m.start()), m.group(1), why))

    for m in STOCK.finditer(clean):
        open_at = m.end() - 1
        own, at = direct_positions(clean, open_at, body_of(clean, open_at))
        if has_prop(own, "contentItem") and (m.group(1) != "ComboBox" or has_prop(own, "delegate")):
            continue
        for key in ("text", "title", "model"):
            v = value_of(clean, own, at, key)
            if v is not None and not is_literal(v):
                findings.append("%s:%d: stock %s draws %s (%s) as AutoText: give it a "
                                "contentItem of plain text" % (name, line(m.start()), m.group(1), key, v[:30]))

    for m in re.finditer(r"(?<![\w.])ToolTip\.text\s*:", clean):
        v = value_of(clean, clean[m.start():], range(m.start(), len(clean)), "ToolTip.text")
        if v is not None and not is_literal(v):
            findings.append("%s:%d: ToolTip.text (%s) draws AutoText" % (name, line(m.start()), v[:30]))

    for m in EDIT.finditer(clean):
        open_at = m.end() - 1
        own, at = direct_positions(clean, open_at, body_of(clean, open_at))
        fmt = value_of(clean, own, at, "textFormat")
        if fmt not in (None, "TextEdit.PlainText", "TextArea.PlainText", "Text.PlainText"):
            findings.append("%s:%d: %s sets textFormat: %s" % (name, line(m.start()), m.group(1), fmt))
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
    def n(src, name="a.qml"):
        return len(scan_text(name, src))

    bad = 'Item {\n  Label { text: x }\n  Text { text: "}" }\n}\n'
    assert n(bad) == 2, scan_text("a.qml", bad)
    good = ('Label { textFormat: Text.PlainText; text: x }\n'
            'Text {\n  textFormat: Text.PlainText\n  Label { textFormat: Text.PlainText }\n}\n')
    assert n(good) == 0, scan_text("a.qml", good)
    # A child's format is not the parent's
    assert n('Label {\n  Text { textFormat: Text.PlainText }\n}\n') == 1
    # Strings and comments hold no elements or formats
    tricky = 'Label {\n  text: "Text { textFormat: x }" // Label {\n  textFormat: Text.PlainText\n}\n'
    assert n(tricky) == 0
    assert n('component Tag: Label { color: "red" }\n') == 1
    # PlainLabel, Text.PlainText and the like are not elements
    assert n("PlainLabel { }\nx: Text.Wrap\n") == 0
    # Only PlainText passes: another value or a binding is a finding
    for fmt in ("Text.AutoText", "Text.StyledText", "Text.RichText", "Text.MarkdownText",
                "rich ? Text.RichText : Text.PlainText"):
        assert n("Label { textFormat: %s }\n" % fmt) == 1, fmt
    # ALLOWED names one format for one (file, objectName)
    ok = 'Text { objectName: "idPayload"; textFormat: Text.StyledText }\n'
    assert n(ok, "FrameIdList.qml") == 0
    assert n(ok, "Other.qml") == 1
    assert n('Text { objectName: "idPayload" }\n', "FrameIdList.qml") == 1
    assert n('Text { objectName: "idPayload"; textFormat: Text.AutoText }\n', "FrameIdList.qml") == 1
    assert n('Text { objectName: "idPayload"; textFormat: Text.PlainText }\n', "FrameIdList.qml") == 1
    # objectName is the element's own: a child's doesn't exempt its parent
    assert n('Text {\n  Item { objectName: "idPayload" }\n}\n', "FrameIdList.qml") == 1
    # A quote in a regex literal doesn't open a string past its line
    rx = 'function q(s) { return s.replace(/"/g, "") }\nLabel { text: m.name }\n'
    assert any("sets no textFormat" in f for f in scan_text("a.qml", rx)), scan_text("a.qml", rx)
    assert any("unterminated" in f for f in scan_text("a.qml", 'x: "abc\nLabel { }\n'))
    # Stock controls: a contentItem of their own, or a literal text
    assert n("Switch { text: m.name }\n") == 1
    assert n('Switch { text: qsTr("On") }\n') == 0
    assert n("Switch { checked: true }\n") == 0
    assert n("CheckBox { text: m.name; contentItem: Label { textFormat: Text.PlainText } }\n") == 0
    assert n("Dialog { title: m.name }\n") == 1
    assert n("MenuItem { text: m.name }\n") == 1
    assert n("ThemedCheckBox { text: m.name }\n") == 0
    assert n("ComboBox { model: names }\n") == 1
    assert n('ComboBox { model: [qsTr("A"), "B"] }\n') == 0
    assert n('ComboBox { model: [{ text: qsTr("A") }] }\n') == 1
    assert n("ComboBox { model: names; contentItem: Text { textFormat: Text.PlainText } }\n") == 1
    assert n("ComboBox { model: names; contentItem: Text { textFormat: Text.PlainText }\n"
             "  delegate: ItemDelegate { contentItem: Text { textFormat: Text.PlainText } } }\n") == 0
    assert n("Item { ToolTip.text: s.unit }\n") == 1
    assert n('Item { ToolTip.text: qsTr("Hi") }\n') == 0
    assert n("ToolTip { text: s.unit }\n") == 1
    # TextEdit and TextArea default to plain; a rich format is a finding
    assert n("TextEdit { }\nTextArea { textFormat: TextEdit.PlainText }\n") == 0
    assert n("TextArea { textFormat: TextEdit.RichText }\n") == 1
    assert n("TextEdit { textFormat: TextEdit.AutoText }\n") == 1
    print("ok")


def main(argv):
    if len(argv) == 2 and argv[1] == "--selftest":
        selftest()
        return 0
    if len(argv) != 2:
        print("usage: plaintext_pin.py <source root> | --selftest")
        return 2
    findings = scan_root(argv[1])
    for f in findings:
        print(f)
    return 1 if findings else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
