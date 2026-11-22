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
#     shrinks or moves, an array taken whole by one field of any type
#     under a new name, at its offset and its size, or a scalar that
#     goes), or one whose type change is a struct that grew this way (a
#     pointer to it, as a set's defs);
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
# reserved, reserved0, display_reserved: not unreserved_count
RESERVED = r"(?:\w+_)?reserved\d*"
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
# A reserved array taken whole by one field, the same bytes at the same
# offset. A field that isn't an array is an entity change, with a size
# line under it only when the size differs; an array of another element
# type says the element changed and that the whole kept its size. The
# new name comes after either
WHOLE_TAKEN = re.compile(r"^entity changed from '\w+\[\d+\]' to '[^']+'$")
ELEMENT_CHANGED = re.compile(r"^array element type '[^']*' changed:$")
SIZE_KEPT = "type size hasn't changed"
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


def moved(members, i):
    """True when the member change at i carries an offset change after it"""
    for later in members[i + 1:]:
        if not later.text.startswith("and "):
            return False
        if OFFSET_TAIL.match(later.text):
            return True
    return False


def struct_grew_into_reserved(node, grown, why):
    """node: a struct-changed line; True when its change is reserved-room growth"""
    name = STRUCT_LINE.match(node.text).group(1)
    size_kept = False
    for child in node.children:
        t = child.text
        if t == SIZE_KEPT:
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
            for i, c in enumerate(child.children):
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
                retyped = any(ELEMENT_CHANGED.match(d.text) for d in c.children)
                kept = any(d.text == SIZE_KEPT for d in c.children)
                whole = retyped and kept
                for detail in c.children:
                    if WHOLE_TAKEN.match(detail.text):
                        whole = True
                        continue
                    if whole and (ELEMENT_CHANGED.match(detail.text) or detail.text == SIZE_KEPT):
                        continue
                    if not ARRAY_DETAIL.match(detail.text):
                        why.append(f"{name}: the reserved member changed otherwise: {detail.text}")
                        return False
                if whole and moved(child.children, i):
                    why.append(f"{name}: a reserved member taken whole moved: {c.text}")
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
# The diagnostics report: clear_kmh took a word of
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
# A reserved array taken whole by an array of another element type, as
# ecuio_recording_signal's key stands where reserved[4] did (abidiff's
# own lines)
RETYPED = """Functions changes summary: 0 Removed, 1 Changed, 0 Added function
Variables changes summary: 0 Removed, 0 Changed, 0 Added variable

1 function with some indirect sub-type change:

  [C] 'function ecuio_status_t ecuio_recording_get_signal(const ecuio_recording_t*, uint32_t, ecuio_recording_signal_t*)' has some indirect sub-type changes:
    parameter 3 of type 'ecuio_recording_signal_t*' has sub-type changes:
      in pointed to type 'typedef ecuio_recording_signal_t':
        underlying type 'struct ecuio_recording_signal' changed:
          type size hasn't changed
          1 data member change:
            type of 'uint64_t reserved[4]' changed:
              array element type 'typedef uint64_t' changed:
                entity changed from 'typedef uint64_t' to 'char'
                type size changed from 64 to 8 (in bits)
              type name changed from 'uint64_t[4]' to 'char[32]'
              type size hasn't changed
              array subrange changed:
                upper bound of '<anonymous range>[4]' change from '3' to '31'
            and name of 'ecuio_recording_signal::reserved' changed to 'ecuio_recording_signal::key'
"""
SCALAR ="""          type size hasn't changed
          1 data member deletion:
            'uint32_t reserved', at offset 32896 (in bits)
          1 data member insertion:
            'uint32_t flags2', at offset 32896 (in bits)
"""


# abidiff's own reports (2.9.0, --no-show-locs) of a struct whose reserved[8]
# a smaller scalar took part of (an insertion beside it; no whole taking),
# and of a field named unreserved_count shrunk like a reserved array
PARTIAL_SCALAR = """Functions changes summary: 0 Removed, 1 Changed, 0 Added function
Variables changes summary: 0 Removed, 0 Changed, 0 Added variable

1 function with some indirect sub-type change:

  [C] 'function int f(s_t*)' has some indirect sub-type changes:
    parameter 1 of type 's_t*' has sub-type changes:
      in pointed to type 'typedef s_t':
        underlying type 'struct s' changed:
          type size hasn't changed
          1 data member insertion:
            'uint8_t more[7]', at offset 40 (in bits)
          1 data member change:
            type of 'uint8_t reserved[8]' changed:
              entity changed from 'uint8_t[8]' to 'typedef uint8_t'
              type size changed from 64 to 8 (in bits)
            and name of 's::reserved' changed to 's::foo'
"""
UNRESERVED = """Functions changes summary: 0 Removed, 1 Changed, 0 Added function
Variables changes summary: 0 Removed, 0 Changed, 0 Added variable

1 function with some indirect sub-type change:

  [C] 'function int f(s_t*)' has some indirect sub-type changes:
    parameter 1 of type 's_t*' has sub-type changes:
      in pointed to type 'typedef s_t':
        underlying type 'struct s' changed:
          type size hasn't changed
          1 data member insertion:
            'uint8_t foo', at offset 32 (in bits)
          1 data member change:
            type of 'uint8_t unreserved_count[8]' changed:
              type name changed from 'uint8_t[8]' to 'uint8_t[7]'
              array type size changed from 64 to 56
              array type subrange 1 changed length from 8 to 7
              array subrange changed: 
                upper bound of '<anonymous range>[8]' change from '7' to '6'
            and offset changed from 32 to 40 (in bits) (by +8 bits)
"""


def field(to, size=""):
    """TAKEN, its one-element array a longer one taken by a field of type
    to that isn't an array; size the line abidiff adds under it when the
    field's size isn't the array's"""
    return TAKEN.replace(
        "            type of 'uint8_t reserved0[1]' changed:\n"
        "              entity changed from 'uint8_t[1]' to 'typedef uint8_t'\n",
        "            type of 'uint8_t reserved0[4]' changed:\n"
        f"              entity changed from 'uint8_t[4]' to '{to}'\n{size}")


def selftest():
    cases = [
        ("growth into reserved[] passes", HEAD + ROOM, True),
        ("a reserved scalar replaced passes", HEAD + SCALAR, True),
        ("a one-element reserved array taken whole passes", TAKEN, True),
        ("a reserved array taken whole by a scalar passes", field("typedef uint32_t"), True),
        ("a reserved array taken whole by a struct passes", field("struct ecuio_span"), True),
        ("a reserved array taken whole by a pointer passes", field("const char*"), True),
        ("a reserved array taken by a field of another size fails", field(
            "typedef uint8_t", "              type size changed from 32 to 8 (in bits)\n"), False),
        ("a reserved array taken whole by a field that moved fails", field("typedef uint32_t").replace(
            "            and name of 'ecuio_channel_diagnostics::reserved0' changed to 'ecuio_channel_diagnostics::cycle'\n",
            "            and name of 'ecuio_channel_diagnostics::reserved0' changed to 'ecuio_channel_diagnostics::cycle'\n"
            "            and offset changed from 192 to 224 (in bits) (by +32 bits)\n"), False),
        ("a reserved array taken whole by another element type passes", RETYPED, True),
        ("a reserved array of another element type and size fails", RETYPED.replace(
            "              type size hasn't changed\n",
            "              array type size changed from 256 to 128\n"), False),
        ("a reserved array of another element type that moved fails", RETYPED
         + "            and offset changed from 768 to 832 (in bits) (by +64 bits)\n", False),
        ("a member that isn't reserved retyped at its size fails", RETYPED.replace("reserved", "name"), False),
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
        ("a smaller scalar taking part of a reserved array fails", PARTIAL_SCALAR, False),
        ("a member only named like reserved fails", UNRESERVED, False),
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
