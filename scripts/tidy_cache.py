#!/usr/bin/env python3
# File/layer: scripts/tidy_cache.py | repo tooling, Python 3.8+, no packages
# Testing: UNIT, over made-up commands and preprocessor output (ctest
# TidyCacheTest.SelfTest); the cache itself by scripts/tidy.sh's runs
#
# clang-tidy's result cache for scripts/tidy.sh: one file's clean run is kept
# under a key, and a later run with the same key replays its output instead of
# running clang-tidy. The key covers everything the result depends on:
#   - the salt tidy.sh passes (clang-tidy's version, the .clang-tidy files'
#     hash, this cache's format),
#   - the file's compile command (less its output names),
#   - the file preprocessed by that command's own compiler (/E or -E),
#   - the bytes of every file the preprocessor read (its line directives),
#     so a change inside a block the compiler skips (#ifdef __clang__) counts.
# Only a run that exits 0 is kept: a warning or a crash always runs again.
# A compile command that won't preprocess leaves the file uncached.
#
# Usage: tidy_cache.py run <clang-tidy> <tree> <file> <log> <cache dir> <salt>
#            runs or replays one file; the log gets clang-tidy's output, and
#            <log>.hit is written on a replay; exits with clang-tidy's status
#        tidy_cache.py selftest
import hashlib
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile

FORMAT = "tidy-cache-1"
# The output names a command writes, which don't touch clang-tidy's result
# (MSVC's -MD picks the runtime: only GCC's is a dependency file)
_DROP_MSVC = re.compile(r'(?<=\s)(?:[/-]F[od]\S*|[/-]showIncludes)(?=\s)')
_DROP_GNU = re.compile(r'(?<=\s)(?:-o\s+\S+|-M[DM]?D|-M[FTQ]\s+\S+)(?=\s)')
_COMPILE_ONLY = re.compile(r'(?<=\s)[/-]c(?=\s)')
_LINE = re.compile(rb'^#(?:line)? \d+ "((?:[^"\\]|\\.)*)"', re.M)


def compiler_of(command):
    """The command's first token, unquoted."""
    command = command.lstrip()
    if command.startswith('"'):
        return command[1:command.index('"', 1)]
    return command.split(None, 1)[0]


def is_msvc(compiler):
    name = os.path.basename(compiler.replace("\\", "/")).lower()
    return name in ("cl", "cl.exe", "clang-cl", "clang-cl.exe")


def preprocess_command(command):
    """The compile command made to preprocess to stdout."""
    msvc = is_msvc(compiler_of(command))
    cmd = (_DROP_MSVC if msvc else _DROP_GNU).sub("", command + " ")
    cmd = _COMPILE_ONLY.sub("", cmd)
    return " ".join(cmd.split()) + (" /E" if msvc else " -E")


def included_paths(text):
    """The files a preprocessed text names in its line directives."""
    out = set()
    for m in _LINE.finditer(text):
        path = m.group(1).decode("utf-8", "replace").replace("\\\\", "\\")
        if not path.startswith("<"):
            out.add(path)
    return sorted(out)


def entry_for(db, path):
    want = os.path.normcase(os.path.normpath(path))
    for e in db:
        f = e["file"]
        if not os.path.isabs(f):
            f = os.path.join(e["directory"], f)
        if os.path.normcase(os.path.normpath(f)) == want:
            return e
    return None


def command_of(entry):
    if "command" in entry:
        return entry["command"]
    return " ".join(shlex.quote(a) for a in entry["arguments"])


def key_of(entry, salt):
    """The file's cache key, or None when its command won't preprocess."""
    cmd = command_of(entry)
    pp = preprocess_command(cmd)
    args = pp if os.name == "nt" else shlex.split(pp)
    try:
        r = subprocess.run(args, cwd=entry["directory"], stdout=subprocess.PIPE,
                           stderr=subprocess.DEVNULL, check=False)
    except OSError:
        return None
    if r.returncode != 0 or not r.stdout:
        return None
    h = hashlib.sha256()
    for part in (FORMAT, salt, entry["file"], pp):
        h.update(part.encode("utf-8") + b"\0")
    h.update(hashlib.sha256(r.stdout).digest())
    for path in included_paths(r.stdout):
        h.update(path.encode("utf-8") + b"\0")
        try:
            with open(path, "rb") as f:
                h.update(hashlib.sha256(f.read()).digest())
        except OSError:
            h.update(b"missing")
    return h.hexdigest()


def store(cache, key, output):
    os.makedirs(cache, exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=cache, suffix=".tmp")
    with os.fdopen(fd, "wb") as f:
        f.write(output)
    os.replace(tmp, os.path.join(cache, key + ".log"))


def run(tidy, tree, path, log, cache, salt):
    with open(os.path.join(tree, "compile_commands.json"), encoding="utf-8") as f:
        entry = entry_for(json.load(f), path)
    key = key_of(entry, salt) if entry else None
    hit = os.path.join(cache, key + ".log") if key else None
    if hit and os.path.isfile(hit):
        with open(hit, "rb") as f:
            output = f.read()
        os.utime(hit)
        with open(log, "wb") as f:
            f.write(output)
        open(log + ".hit", "wb").close()
        return 0
    # CreateProcess can't start a relative path written with slashes
    exe = os.path.abspath(tidy) if os.path.isfile(tidy) else (shutil.which(tidy) or tidy)
    r = subprocess.run([exe, "-p", tree, "--quiet", path], stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, check=False)
    with open(log, "wb") as f:
        f.write(r.stdout)
    if r.returncode == 0 and key:
        store(cache, key, r.stdout)
    return r.returncode


def selftest():
    cl = ('"C:\\Program Files\\VC\\cl.exe"  /nologo /TP -DA -IE:\\src /O2 '
          '/Foapp\\x.cpp.obj /Fdapp\\x.pdb /FS -c E:\\src\\x.cpp')
    assert compiler_of(cl) == "C:\\Program Files\\VC\\cl.exe"
    assert preprocess_command(cl) == ('"C:\\Program Files\\VC\\cl.exe" /nologo /TP -DA '
                                      '-IE:\\src /O2 /FS E:\\src\\x.cpp /E'), preprocess_command(cl)
    dash = '"cl.exe" -DA -MD -Fox.obj -Fdx.pdb -c x.cpp'
    assert preprocess_command(dash) == '"cl.exe" -DA -MD x.cpp /E', preprocess_command(dash)
    gcc = "/usr/bin/c++ -DA -I/src -O2 -MD -MT x.o -MF x.o.d -o x.o -c /src/x.cpp"
    assert preprocess_command(gcc) == "/usr/bin/c++ -DA -I/src -O2 /src/x.cpp -E", preprocess_command(gcc)
    # -c inside a token stays
    assert "-cfg" in preprocess_command("cc -cfg -c x.c")
    text = (b'#line 1 "E:\\\\src\\\\x.cpp"\nint a;\n#line 1 "C:\\\\Qt\\\\q.h"\n'
            b'# 3 "/usr/include/s.h" 1 3\n#line 4 "<built-in>"\n  #line 9 "no.h"\n'
            b'#line 2 "E:\\\\src\\\\x.cpp"\n')
    assert included_paths(text) == ["/usr/include/s.h", "C:\\Qt\\q.h", "E:\\src\\x.cpp"], included_paths(text)
    db = [{"directory": "/t", "file": "a/b.cpp", "command": "cc -c a/b.cpp"}]
    assert entry_for(db, "/t/a/b.cpp") is db[0] and entry_for(db, "/t/a/c.cpp") is None
    assert command_of({"arguments": ["cc", "-DX=a b", "-c", "f.c"]}) == "cc '-DX=a b' -c f.c"
    print("tidy_cache selftest ok")
    return 0


def main(argv):
    if len(argv) == 2 and argv[1] == "selftest":
        return selftest()
    if len(argv) == 8 and argv[1] == "run":
        return run(*argv[2:])
    sys.stderr.write(__doc__ if __doc__ else "usage: see the top of tidy_cache.py\n")
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
