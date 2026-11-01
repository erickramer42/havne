# File/layer: scripts/abi_report.py | Python 3.8+, no packages
# Testing: UNIT, its own self-test (ctest AbiReportTest.SelfTest), over
#          abidiff reports of real mutants; run by scripts/abi-check.sh
#
# Judges an abidiff report against the C ABI's growth rule (ADR-0001): a
# public struct grows into its reserved room. abidiff reports that as a
# changed function, and a libabigail suppression of the type would hide
# every other change to the struct with it, so the report is read here
# instead. A report passes when every changed function's changes come
# down to structs that:
#   - keep their size;
#   - change or delete no member but one named reserved (an array that
#     shrinks or moves, a one-element array taken whole as its element
#     under a new name, or a scalar that goes), or one whose type change
#     is a struct that grew this way (a pointer to it, as a set's defs);
#   - otherwise only gain members (insertions).
# So no field outside the reserved room moves, resizes or changes type.
# Anything else in a changed function (an enum, a return type, a struct
# that grew) fails, and so does any removed function or changed variable.
#
#   python3 scripts/abi_report.py <report.txt>   exit 0: growth only, 1: not
#   python3 scripts/abi_report.py selftest

import re
import sys

# A step on the way to a changed type ("parameter 1 of type …", "in
# pointed to type ...", "in unqualified underlying type ...", a function
# pointer's type): it passes when everything under it does. What
# changed is said by the lines under the steps, and a change that isn't
# a struct's (an enumerator, a size, a typedef's name) is a line no
# rule below accepts
PATH_LINE = re.compile(r"^.*:$")
STRUCT_LINE = re.compile(r"^(?:underlying )?type 'struct (\w+)' changed:$")
EARLIER = re.compile(r"^type 'struct (\w+)' changed, as reported earlier$")
RESERVED = r"\w*reserved\w*"
MEMBER_TYPE_CHANGE = re.compile(rf"^type of '[^']*\b{RESERVED}(\[\d+\])?' changed:$")
ANY_MEMBER_TYPE_CHANGE = re.compile(r"^type of '[^']*' changed:$")
MEMBER_GONE = re.compile(rf"^'[^']*\b{RESERVED}(\[\d+\])?', at offset \d+ \(in bits\)$")
INSERTED = re.compile(r"^'[^']*', at offset \d+ \(in bits\)$")
ARRAY_DETAIL = re.compile(
    r"^(type name changed from '\w+\[\d+\]' to '\w+\[\d+\]'"
    r"|array type size changed from \d+ to \d+"
    r"|array type subrange \d+ changed length from \d+ to \d+"
    r"|array subrange changed: ?"
    r"|upper bound of '.*' change from '\d+' to '\d+')$"
)
OFFSET_TAIL = re.compile(r"^and offset changed from \d+ to \d+ \(in bits\) \(by [+-]\d+ bits\)$")
# A one-element reserved array taken whole: its element's type, the same
# bytes at the same offset, and the new name abidiff gives after it
ONE_TAKEN = re.compile(r"^entity changed from '(\w+)\[1\]' to '(?:typedef )?(\w+)'$")
RESERVED_RENAMED = re.compile(rf"^and name of '[\w:]*::{RESERVED}' changed to '[\w:]*::\w+'$")


class Node:
    def __init__(self, indent, text):
        self.indent, self.text, self.children = indent, text, []


def tree(lines):
    """The report's indented lines as a forest"""
    root = Node(-1, "")
    stack = [root]
    for raw in lines:
        if not raw.strip():
            continue
        indent = len(raw) - len(raw.lstrip(" "))
        node = Node(indent, raw.strip())
        while stack[-1].indent >= indent:
            stack.pop()
        stack[-1].children.append(node)
        stack.append(node)
    return root.children


def struct_grew_into_reserved(node, grown, why):
    """node: a struct-changed line; True when its change is reserved-room growth"""
    name = STRUCT_LINE.match(node.text).group(1)
    size_kept = False
    for child in node.children:
        t = child.text
        if t == "type size hasn't changed":
            size_kept = True
        elif re.match(r"^\d+ data member insertions?:$", t):
            if not all(INSERTED.match(c.text) for c in child.children):
                why.append(f"{name}: an insertion abidiff describes oddly")
                return False
        elif re.match(r"^\d+ data member deletions?:$", t):
            for c in child.children:
                if not MEMBER_GONE.match(c.text):
                    why.append(f"{name}: a member other than reserved deleted: {c.text}")
                    return False
        elif re.match(r"^\d+ data member changes?:$", t):
            for c in child.children:
                if OFFSET_TAIL.match(c.text):
                    continue  # the reserved member's own move, after its type
                if RESERVED_RENAMED.match(c.text):
                    continue  # its new name, after a whole one taken
                if (not MEMBER_TYPE_CHANGE.match(c.text)
                        and ANY_MEMBER_TYPE_CHANGE.match(c.text) and c.children
                        and all(PATH_LINE.match(d.text) for d in c.children)):
                    # another member, its type a path to a changed struct:
                    # it passes when that struct grew into its room
                    if all(explained(d, grown, why) for d in c.children):
                        continue
                    return False
                if not MEMBER_TYPE_CHANGE.match(c.text):
                    why.append(f"{name}: {c.text}")
                    return False
                for detail in c.children:
                    taken = ONE_TAKEN.match(detail.text)
                    if taken and taken.group(1) == taken.group(2):
                        continue
                    if not ARRAY_DETAIL.match(detail.text):
                        why.append(f"{name}: the reserved member changed otherwise: {detail.text}")
                        return False
        else:
            why.append(f"{name}: {t}")
            return False
    if not size_kept:
        why.append(f"{name}: its size changed")
        return False
    return True


def explained(node, grown, why):
    """True when everything under node is reserved-room growth"""
    t = node.text
    struct = STRUCT_LINE.match(t)
    if struct:
        ok = struct_grew_into_reserved(node, grown, why)
        if ok:
            grown.add(struct.group(1))
        return ok
    earlier = EARLIER.match(t)
    if earlier:
        if earlier.group(1) in grown:
            return True
        why.append(f"{earlier.group(1)}: changed, as reported earlier")
        return False
    if PATH_LINE.match(t) and node.children:
        return all(explained(c, grown, why) for c in node.children)
    why.append(t)
    return False


def judge(text):
    """(passes, the reasons it doesn't, the structs that grew)"""
    why, grown = [], set()
    functions = re.search(r"Functions changes summary: (\d+) Removed, (\d+) Changed", text)
    variables = re.search(r"Variables changes summary: (\d+) Removed, (\d+) Changed", text)
    if not functions or not variables:
        return False, ["not an abidiff report (no changes summary)"], grown
    removed, changed = int(functions.group(1)), int(functions.group(2))
    if removed:
        why.append(f"{removed} function(s) removed")
    if int(variables.group(1)) or int(variables.group(2)):
        why.append("a variable removed or changed")
    if why or changed == 0:
        return not why, why, grown
    start = text.find("with some indirect sub-type change")
    if start < 0:
        return False, ["changed functions abidiff doesn't describe as sub-type changes"], grown
    body = text[text.find("\n", start) + 1:]
    stop = re.search(r"^\d+ (Added|Removed) (function|variable)", body, re.M)
    lines = (body[:stop.start()] if stop else body).splitlines()
    functions_seen = [n for n in tree(lines) if n.text.startswith("[C] ")]
    if len(functions_seen) != changed:
        why.append(f"{changed} changed function(s), {len(functions_seen)} described")
    for f in functions_seen:
        explained(f, grown, why)
    return not why, why, grown


HEAD = """Functions changes summary: 0 Removed, 1 Changed, 0 Added function
Variables changes summary: 0 Removed, 0 Changed, 0 Added variable

1 function with some indirect sub-type change:

  [C] 'function ecuio_status_t ecuio_serial_ports(ecuio_serial_port_t*, uint32_t, uint32_t*)' has some indirect sub-type changes:
    parameter 1 of type 'ecuio_serial_port_t*' has sub-type changes:
      in pointed to type 'typedef ecuio_serial_port_t':
        underlying type 'struct ecuio_serial_port' changed:
"""
ROOM = """          type size hasn't changed
          1 data member insertion:
            'uint32_t baud_hint', at offset 2080 (in bits)
          1 data member change:
            type of 'uint32_t reserved[7]' changed:
              type name changed from 'uint32_t[7]' to 'uint32_t[6]'
              array type size changed from 224 to 192
              array type subrange 1 changed length from 7 to 6
              array subrange changed:
                upper bound of '<anonymous range>[7]' change from '6' to '5'
            and offset changed from 2080 to 2112 (in bits) (by +32 bits)
"""
SWAP = """          type size hasn't changed
          1 data member insertion:
            'uint32_t baud_hint', at offset 2080 (in bits)
          3 data member changes:
            'char name[128]' offset changed from 0 to 1024 (in bits) (by +1024 bits)
            'char device[128]' offset changed from 1024 to 0 (in bits) (by -1024 bits)
            type of 'uint32_t reserved[7]' changed:
              type name changed from 'uint32_t[7]' to 'uint32_t[6]'
              array type size changed from 224 to 192
            and offset changed from 2080 to 2112 (in bits) (by +32 bits)
"""
SPLIT = """          type size hasn't changed
          1 data member insertion:
            'uint16_t flags_hi', at offset 2064 (in bits)
          1 data member change:
            type of 'uint32_t flags' changed:
              typedef name changed from uint32_t to uint16_t
"""
GROWN = """          type size changed from 2304 to 2336 (in bits)
          1 data member insertion:
            'uint32_t baud_hint', at offset 2080 (in bits)
"""
# A set's pointer member to a def that grew into its room, as
# ecuio_signal_set's defs did when plot_rank took a reserved byte
NESTED = """Functions changes summary: 0 Removed, 1 Changed, 0 Added function
Variables changes summary: 0 Removed, 0 Changed, 0 Added variable

1 function with some indirect sub-type change:

  [C] 'function ecuio_status_t ecuio_profile_register(const char*, const ecuio_signal_set_t*, ecuio_validation_t*)' has some indirect sub-type changes:
    parameter 2 of type 'const ecuio_signal_set_t*' has sub-type changes:
      in pointed to type 'const ecuio_signal_set_t':
        in unqualified underlying type 'typedef ecuio_signal_set_t':
          underlying type 'struct ecuio_signal_set' changed:
            type size hasn't changed
            1 data member change:
              type of 'const ecuio_signal_def_t* defs' changed:
                in pointed to type 'const ecuio_signal_def_t':
                  in unqualified underlying type 'typedef ecuio_signal_def_t':
                    underlying type 'struct ecuio_signal_def' changed:
                      type size hasn't changed
                      1 data member insertion:
                        'uint8_t plot_rank', at offset 712 (in bits)
                      1 data member change:
                        type of 'uint8_t display_reserved[3]' changed:
                          type name changed from 'uint8_t[3]' to 'uint8_t[2]'
                          array type size changed from 24 to 16
                          array type subrange 1 changed length from 3 to 2
                        and offset changed from 712 to 720 (in bits) (by +8 bits)
"""
# The diagnostics report as v0.3.6 grew it: clear_kmh took a word of
# reserved[3], and cycle took reserved0[1] whole (abidiff's own lines)
TAKEN = """Functions changes summary: 0 Removed, 1 Changed, 1 Added functions
Variables changes summary: 0 Removed, 0 Changed, 0 Added variable

1 function with some indirect sub-type change:

  [C] 'function ecuio_status_t ecuio_session_get_channel_diagnostics(const ecuio_session_t*, uint32_t, ecuio_channel_diagnostics_t*)' has some indirect sub-type changes:
    parameter 3 of type 'ecuio_channel_diagnostics_t*' has sub-type changes:
      in pointed to type 'typedef ecuio_channel_diagnostics_t':
        underlying type 'struct ecuio_channel_diagnostics' changed:
          type size hasn't changed
          1 data member insertion:
            'uint32_t clear_kmh', at offset 224 (in bits)
          2 data member changes:
            type of 'uint8_t reserved0[1]' changed:
              entity changed from 'uint8_t[1]' to 'typedef uint8_t'
            and name of 'ecuio_channel_diagnostics::reserved0' changed to 'ecuio_channel_diagnostics::cycle'
            type of 'uint32_t reserved[3]' changed:
              type name changed from 'uint32_t[3]' to 'uint32_t[2]'
              array type size changed from 96 to 64
              array type subrange 1 changed length from 3 to 2
            and offset changed from 224 to 256 (in bits) (by +32 bits)

1 Added function:

  [A] 'function ecuio_status_t ecuio_session_clear_diagnostics_with(ecuio_session_t*, uint32_t, uint32_t, uint32_t)'    {ecuio_session_clear_diagnostics_with}
"""
SCALAR = """          type size hasn't changed
          1 data member deletion:
            'uint32_t reserved', at offset 32896 (in bits)
          1 data member insertion:
            'uint32_t flags2', at offset 32896 (in bits)
"""


def selftest():
    cases = [
        ("growth into reserved[] passes", HEAD + ROOM, True),
        ("a reserved scalar replaced passes", HEAD + SCALAR, True),
        ("a one-element reserved array taken whole passes", TAKEN, True),
        ("a one-element reserved array taken as another type fails", TAKEN.replace(
            "to 'typedef uint8_t'", "to 'typedef uint16_t'"), False),
        ("a member outside the room renamed fails", TAKEN.replace(
            "            and name of 'ecuio_channel_diagnostics::reserved0' changed to 'ecuio_channel_diagnostics::cycle'\n",
            "            and name of 'ecuio_channel_diagnostics::reserved0' changed to 'ecuio_channel_diagnostics::cycle'\n"
            "            and name of 'ecuio_channel_diagnostics::clear_nrc' changed to 'ecuio_channel_diagnostics::nrc'\n"), False),
        ("a pointer member to a struct grown into its room passes", NESTED, True),
        ("a pointer member to a struct that grew fails", NESTED.replace(
            "                      type size hasn't changed\n",
            "                      type size changed from 960 to 968 (in bits)\n"), False),
        ("a pointer member retyped fails", NESTED.replace(
            "                in pointed to type 'const ecuio_signal_def_t':\n"
            "                  in unqualified underlying type 'typedef ecuio_signal_def_t':\n",
            "                in pointed to type 'const ecuio_signal_def_t':\n"
            "                  typedef name changed from ecuio_signal_def_t to ecuio_other_t\n"
            "                  in unqualified underlying type 'typedef ecuio_signal_def_t':\n"), False),
        ("fields swapped beside the growth fail", HEAD + SWAP, False),
        ("a field split before the room fails", HEAD + SPLIT, False),
        ("a struct that grew fails", HEAD + GROWN, False),
        ("a removed function fails", HEAD.replace("0 Removed, 1 Changed", "1 Removed, 1 Changed") + ROOM, False),
        ("a changed variable fails", HEAD.replace("0 Removed, 0 Changed, 0 Added variable",
                                                  "0 Removed, 1 Changed, 0 Added variable") + ROOM, False),
        ("an enum change fails", HEAD.replace("underlying type 'struct ecuio_serial_port' changed:",
                                              "underlying type 'enum ecuio_status' changed:") + ROOM, False),
        ("an unexplained second function fails", HEAD.replace("0 Removed, 1 Changed,", "0 Removed, 2 Changed,") + ROOM, False),
        ("a function pointer's path to a grown struct passes", HEAD.replace(
            "      in pointed to type 'typedef ecuio_serial_port_t':\n",
            "      in pointed to type 'function type void (const ecuio_serial_port_t*)':\n"
            "       in unqualified underlying type 'typedef ecuio_serial_port_t':\n") + ROOM, True),
        ("an enumerator change on the path fails", HEAD.replace(
            "        underlying type 'struct ecuio_serial_port' changed:\n",
            "        underlying type 'enum ecuio_status' changed:\n"
            "          type size hasn't changed\n"
            "          1 enumerator change:\n"
            "            'ECUIO_ERR_NULL_ARGUMENT' from value '1' to '41'\n"), False),
        ("additions only pass", "Functions changes summary: 0 Removed, 0 Changed, 3 Added functions\n"
                                "Variables changes summary: 0 Removed, 0 Changed, 1 Added variable\n", True),
        ("not a report fails", "abidiff: something went wrong\n", False),
    ]
    bad = []
    for name, text, want in cases:
        got, why, _ = judge(text)
        if got != want:
            bad.append(f"{name}: {'passed' if got else 'failed'} ({'; '.join(why)})")
    for b in bad:
        print(f"FAIL  {b}")
    print(f"{len(cases) - len(bad)}/{len(cases)} checks passed")
    return 1 if bad else 0


def main():
    if len(sys.argv) == 2 and sys.argv[1] == "selftest":
        return selftest()
    if len(sys.argv) != 2:
        print(__doc__ if __doc__ else "abi_report.py <report.txt> | selftest", file=sys.stderr)
        return 2
    with open(sys.argv[1], encoding="utf-8") as f:
        ok, why, grown = judge(f.read())
    for w in why:
        print(f"FAIL  {w}")
    if ok and grown:
        print("ok    grew into the reserved room: " + ", ".join(sorted(grown)))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
