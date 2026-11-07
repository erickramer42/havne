# File/layer: packaging/python/make_wheel.py | release packaging, Python 3.8+, no packages
# Testing: UNIT, its own self-test (ctest WheelTest.SelfTest); HARNESS,
#          WheelTest.Installed (tests/wheel_installed.py) over the tree's
#          own library
#
# The havne wheel (ADR-0053 section 6 and its appendix "the wheel as
# built"): python/havne/ and the shared core in one platform wheel,
# havne-<version>-py3-none-<platform>.whl, written with the standard
# library alone (a wheel is a zip with METADATA, WHEEL and RECORD), so
# there is no build backend to pin. On Windows the two J2534 surrogates
# go beside ecuio.dll, which the package hands core at import.
#
#   python3 packaging/python/make_wheel.py build --version 0.6.0 \
#       --library build/core/libecuio.so.0.6.0 --platform-tag manylinux_2_39_x86_64 --out dist
#   python make_wheel.py build --version 0.6.0 --library build/core/Release/ecuio.dll \
#       --surrogate-x86 build-win32/surrogate/Release/ecuio-j2534-surrogate.exe \
#       --surrogate-x64 build/surrogate/Release/ecuio-j2534-surrogate-x64.exe --out dist
#   python3 packaging/python/make_wheel.py selftest
#
# The platform tag defaults to what the library itself says ("auto": its
# machine, the newest glibc symbol version it needs, its macOS minos); a
# tag given is checked against the library and refused when it claims
# more than the library can keep. Refused too: a _version.py or a
# library of another version, a Windows binary that imports the C
# runtime (the library and the surrogates link it in), a missing or
# misnamed surrogate, a Linux library needing a library outside
# manylinux's set, a macOS one needing one outside the system's. The
# same inputs and SOURCE_DATE_EPOCH give the same bytes. Beside the
# wheel, <wheel>.components.json for the release's SBOM
# (packaging/sbom.py): a job artifact, never a release asset.
#
# Exit 0 on success, 1 on a failed self-test, 2 on a refusal or error.

import argparse
import base64
import ctypes
import hashlib
import json
import os
import re
import struct
import sys
import tempfile
import time
import zipfile

sys.dont_write_bytecode = True

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
NAME = "havne"
SUMMARY = "Havne's ECU reader core from Python: sessions, recordings and the simulated ECU"
HOME = "https://github.com/erickramer42/havne"
REQUIRES_PYTHON = ">=3.11"
SURROGATES = (("x86", "ecuio-j2534-surrogate.exe", 0x14C),
              ("x64", "ecuio-j2534-surrogate-x64.exe", 0x8664))
LICENSES = (("LICENSE", "LICENSE"), ("core/THIRD_PARTY_NOTICES.txt", "THIRD_PARTY_NOTICES.txt"))

CRT_IMPORT = re.compile(r"^(vcruntime|msvcp|ucrtbase|concrt|vccorlib|api-ms-win-crt)", re.I)
MANYLINUX_LIBS = {"libc.so.6", "libm.so.6", "libgcc_s.so.1", "libstdc++.so.6",
                  "libpthread.so.0", "libdl.so.2", "librt.so.1",
                  "ld-linux-x86-64.so.2", "ld-linux-aarch64.so.1"}
MACOS_LIB_DIRS = ("/usr/lib/", "/System/Library/")
PE_MACHINES = {0x8664: "win_amd64", 0x14C: "win32", 0xAA64: "win_arm64"}
ELF_MACHINES = {62: "x86_64", 183: "aarch64"}
MACHO_CPUS = {0x0100000C: "arm64", 0x01000007: "x86_64"}


class Refused(Exception):
    pass


# ---- the binaries, read with the standard library

def _u16(b, o):
    return struct.unpack_from("<H", b, o)[0]


def _u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


def _u64(b, o):
    return struct.unpack_from("<Q", b, o)[0]


def _cstr(b, o):
    end = b.index(b"\0", o)
    return b[o:end].decode("utf-8", "replace")


def kind_of(data):
    if data[:2] == b"MZ":
        return "pe"
    if data[:4] == b"\x7fELF":
        return "elf"
    if data[:4] == struct.pack("<I", 0xFEEDFACF):
        return "macho"
    raise Refused("not a PE, ELF or 64-bit Mach-O file")


def pe_info(data):
    """(machine, the DLLs it imports)"""
    pe = _u32(data, 0x3C)
    if data[pe:pe + 4] != b"PE\0\0":
        raise Refused("a PE file without its PE header")
    machine, nsec = _u16(data, pe + 4), _u16(data, pe + 6)
    opt_size, opt = _u16(data, pe + 20), pe + 24
    magic = _u16(data, opt)
    dirs = opt + (96 if magic == 0x10B else 112)
    imp_rva = _u32(data, dirs + 8)
    secs = [struct.unpack_from("<IIII", data, opt + opt_size + 40 * i + 8) for i in range(nsec)]

    def offset(rva):
        for vsize, va, rawsize, raw in secs:
            if va <= rva < va + max(vsize, rawsize):
                return raw + rva - va
        raise Refused(f"a PE address 0x{rva:x} outside its sections")

    names = []
    if imp_rva:
        d = offset(imp_rva)
        while True:
            fields = struct.unpack_from("<IIIII", data, d)
            if not any(fields):
                break
            names.append(_cstr(data, offset(fields[3])))
            d += 20
    return machine, names


def elf_info(data):
    """(machine, NEEDED, SONAME, the newest GLIBC_ version needed as a
    tuple); 64-bit little-endian only"""
    if data[4] != 2 or data[5] != 1:
        raise Refused("an ELF file that isn't 64-bit little-endian")
    machine = _u16(data, 18)
    shoff, shentsize, shnum = _u64(data, 0x28), _u16(data, 0x3A), _u16(data, 0x3C)
    secs = []
    for i in range(shnum):
        o = shoff + i * shentsize
        secs.append((_u32(data, o + 4), _u64(data, o + 0x18), _u64(data, o + 0x20),
                     _u32(data, o + 0x28)))
    needed, soname, glibc = [], "", (0, 0)
    for sh_type, off, size, link in secs:
        if sh_type == 6:                      # SHT_DYNAMIC
            strtab = secs[link][1]
            for e in range(off, off + size, 16):
                tag, val = struct.unpack_from("<qQ", data, e)
                if tag == 1:
                    needed.append(_cstr(data, strtab + val))
                elif tag == 14:
                    soname = _cstr(data, strtab + val)
        elif sh_type == 0x6FFFFFFE:           # SHT_GNU_verneed
            strtab, vn = secs[link][1], off
            while True:
                _, cnt, _, aux, nxt = struct.unpack_from("<HHIII", data, vn)
                a = vn + aux
                for _ in range(cnt):
                    _, _, _, name, anext = struct.unpack_from("<IHHII", data, a)
                    m = re.fullmatch(r"GLIBC_(\d+)\.(\d+)(?:\.\d+)?", _cstr(data, strtab + name))
                    if m:
                        glibc = max(glibc, (int(m.group(1)), int(m.group(2))))
                    a += anext
                if nxt == 0:
                    break
                vn += nxt
    return machine, needed, soname, glibc


def macho_info(data):
    """(cpu type, minos as (major, minor), the dylibs it loads)"""
    cpu, ncmds = struct.unpack_from("<i", data, 4)[0] & 0xFFFFFFFF, _u32(data, 16)
    minos, dylibs, o = None, [], 32
    for _ in range(ncmds):
        cmd, size = _u32(data, o), _u32(data, o + 4)
        if cmd == 0x32:                       # LC_BUILD_VERSION
            v = _u32(data, o + 12)
            minos = (v >> 16, (v >> 8) & 0xFF)
        elif cmd == 0x24 and minos is None:   # LC_VERSION_MIN_MACOSX
            v = _u32(data, o + 8)
            minos = (v >> 16, (v >> 8) & 0xFF)
        elif cmd in (0xC, 0x80000018, 0x8000001F):   # LC_LOAD_DYLIB, _WEAK_, LC_REEXPORT_
            dylibs.append(_cstr(data, o + _u32(data, o + 8)))
        o += size
    if minos is None:
        raise Refused("a Mach-O file with no minimum macOS")
    return cpu, minos, dylibs


# ---- platform tags

def auto_tag(data):
    kind = kind_of(data)
    if kind == "pe":
        machine, _ = pe_info(data)
        if machine not in PE_MACHINES:
            raise Refused(f"a PE machine 0x{machine:x} with no wheel tag")
        return PE_MACHINES[machine]
    if kind == "elf":
        machine, _, _, glibc = elf_info(data)
        if machine not in ELF_MACHINES:
            raise Refused(f"an ELF machine {machine} with no wheel tag")
        glibc = max(glibc, (2, 17))
        return f"manylinux_{glibc[0]}_{glibc[1]}_{ELF_MACHINES[machine]}"
    cpu, (major, minor), _ = macho_info(data)
    if cpu not in MACHO_CPUS:
        raise Refused(f"a Mach-O CPU 0x{cpu:x} with no wheel tag")
    if major >= 11:   # macOS 11 and later: the major alone (check_library says why)
        return f"macosx_{major}_0_{MACHO_CPUS[cpu]}"
    return f"macosx_{major}_{minor}_{MACHO_CPUS[cpu]}"


def check_library(data, tag):
    """The library fits tag ("auto": the library's own) and needs nothing
    the wheel can't count on; the tag"""
    kind = kind_of(data)
    own = auto_tag(data)
    if tag == "auto":
        tag = own
    if kind == "pe":
        _, imports = pe_info(data)
        crt = [i for i in imports if CRT_IMPORT.match(i)]
        if crt:
            raise Refused(f"the library imports the C runtime ({', '.join(crt)}): "
                          "it must link it in (/MT), as the surrogates do")
        if tag != own:
            raise Refused(f"tag {tag}, but the library is {own}")
    elif kind == "elf":
        _, needed, soname, glibc = elf_info(data)
        if not soname:
            raise Refused("the library has no SONAME")
        outside = sorted(set(needed) - MANYLINUX_LIBS)
        if outside:
            raise Refused(f"the library needs {', '.join(outside)}, outside manylinux's set")
        m = re.fullmatch(r"manylinux_(\d+)_(\d+)_(\w+)", tag)
        if not m or m.group(3) != own.split("_", 3)[3]:
            raise Refused(f"tag {tag}, but the library is {own}")
        if (int(m.group(1)), int(m.group(2))) < glibc:
            raise Refused(f"tag {tag}, but the library needs GLIBC_{glibc[0]}.{glibc[1]}")
    else:
        _, minos, dylibs = macho_info(data)
        outside = [d for d in dylibs if not d.startswith(MACOS_LIB_DIRS)]
        if outside:
            raise Refused(f"the library needs {', '.join(outside)}, outside the system's")
        m = re.fullmatch(r"macosx_(\d+)_(\d+)_(\w+)", tag)
        if not m or m.group(3) != own.split("_", 3)[3]:
            raise Refused(f"tag {tag}, but the library is {own}")
        want = (int(m.group(1)), int(m.group(2)))
        # From macOS 11 pip offers a machine only <major>_0 tags, so a tag
        # names the major alone: a library built for 14.4 is macosx_14_0
        # (rounding up would refuse it on 14.x itself), and installs on
        # 14.0 to 14.3 too, where its load fails. A release builds for
        # an exact major (13.0), so its wheel claims no more than it keeps
        if want[0] >= 11 and want[1] != 0:
            raise Refused(f"tag {tag}: from macOS 11 a tag is <major>_0")
        need = (minos[0], 0) if minos[0] >= 11 else minos
        if want < need:
            raise Refused(f"tag {tag}, but the library needs macOS {minos[0]}.{minos[1]}")
    return tag


def check_surrogate(path, name, machine):
    with open(path, "rb") as f:
        data = f.read()
    if kind_of(data) != "pe":
        raise Refused(f"{name}: not a PE file")
    got, imports = pe_info(data)
    if got != machine:
        raise Refused(f"{name}: machine 0x{got:x}, not 0x{machine:x}")
    crt = [i for i in imports if CRT_IMPORT.match(i)]
    if crt:
        raise Refused(f"{name} imports the C runtime ({', '.join(crt)})")
    return data


# ---- the version

def pep440(version):
    m = re.fullmatch(r"(\d+)\.(\d+)\.(\d+)(?:-rc(\d+))?", version)
    if not m:
        raise Refused(f"version {version!r} isn't X.Y.Z or X.Y.Z-rcN")
    base = ".".join(m.group(1, 2, 3))
    return base + (f"rc{m.group(4)}" if m.group(4) else ""), base


def package_version(package_dir):
    with open(os.path.join(package_dir, "_version.py"), encoding="utf-8") as f:
        m = re.search(r'^__version__ = "([0-9.]+)"$', f.read(), re.M)
    if not m:
        raise Refused("_version.py has no __version__")
    return m.group(1)


def loaded_version(path):
    """ecuio_version of the library, loaded here"""
    try:
        lib = ctypes.CDLL(os.path.abspath(path))
    except OSError as e:
        raise Refused(f"the library doesn't load here to read its version: {e}") from e
    v = [ctypes.c_uint32() for _ in range(3)]
    lib.ecuio_version(*[ctypes.byref(x) for x in v])
    return ".".join(str(x.value) for x in v)


# ---- the wheel

def _record_hash(data):
    return "sha256=" + base64.urlsafe_b64encode(hashlib.sha256(data).digest()).rstrip(b"=").decode()


def _date_time(epoch):
    t = time.gmtime(max(epoch, 315532800))   # zip's floor, 1980-01-01
    return t[:6]


def build(version, library, out, tag="auto", surrogates=None, package_dir=None,
          licenses=None, epoch=None, version_of=loaded_version, missing_surrogates_ok=False):
    """Writes the wheel and its component list into out; the wheel's path.
    missing_surrogates_ok is the wheel test's alone (a tree with no x86
    surrogate, CI's win-build): the command line never sets it"""
    package_dir = package_dir or os.path.join(ROOT, "python", NAME)
    licenses = licenses if licenses is not None else [(os.path.join(ROOT, a), b) for a, b in LICENSES]
    if epoch is None:
        epoch = int(os.environ.get("SOURCE_DATE_EPOCH", "0") or 0)
    wheel_version, base = pep440(version)
    if package_version(package_dir) != base:
        raise Refused(f"_version.py says {package_version(package_dir)}, not {base}")
    with open(library, "rb") as f:
        lib = f.read()
    tag = check_library(lib, tag)
    kind = kind_of(lib)
    if version_of is not None and version_of(library) != base:
        raise Refused(f"the library is core {version_of(library)}, not {base}")

    files = []   # (name in the wheel, bytes, executable)
    for entry in sorted(os.listdir(package_dir)):
        path = os.path.join(package_dir, entry)
        if entry.endswith(".py") and os.path.isfile(path):
            with open(path, "rb") as f:
                files.append((f"{NAME}/{entry}", f.read(), False))
    if not any(n == f"{NAME}/__init__.py" for n, _, _ in files):
        raise Refused(f"{package_dir} has no __init__.py")
    if kind == "pe":
        files.append((f"{NAME}/ecuio.dll", lib, True))
        surrogates = surrogates or {}
        for arch, name, machine in SURROGATES:
            if not surrogates.get(arch):
                if missing_surrogates_ok:
                    continue
                raise Refused(f"a Windows wheel needs the {arch} surrogate ({name})")
            files.append((f"{NAME}/{name}", check_surrogate(surrogates[arch], name, machine), True))
    elif surrogates:
        raise Refused("surrogates go only in a Windows wheel")
    elif kind == "elf":
        files.append((f"{NAME}/{elf_info(lib)[2]}", lib, True))
    else:
        files.append((f"{NAME}/libecuio.dylib", lib, True))

    dist = f"{NAME}-{wheel_version}.dist-info"
    meta = ["Metadata-Version: 2.1", f"Name: {NAME}", f"Version: {wheel_version}",
            f"Summary: {SUMMARY}", f"Home-page: {HOME}", "License: MIT",
            f"Requires-Python: {REQUIRES_PYTHON}"]
    files.append((f"{dist}/METADATA", ("\n".join(meta) + "\n").encode(), False))
    wheel = ["Wheel-Version: 1.0", "Generator: havne make_wheel.py", "Root-Is-Purelib: false",
             f"Tag: py3-none-{tag}"]
    files.append((f"{dist}/WHEEL", ("\n".join(wheel) + "\n").encode(), False))
    for src, name in licenses:
        with open(src, "rb") as f:
            files.append((f"{dist}/licenses/{name}", f.read(), False))
    files.sort(key=lambda f: f[0])
    record = "".join(f"{n},{_record_hash(d)},{len(d)}\n" for n, d, _ in files)
    record += f"{dist}/RECORD,,\n"
    files.append((f"{dist}/RECORD", record.encode(), False))

    os.makedirs(out, exist_ok=True)
    whl = os.path.join(out, f"{NAME}-{wheel_version}-py3-none-{tag}.whl")
    partial = whl + ".partial"
    when = _date_time(epoch)
    try:
        with zipfile.ZipFile(partial, "w") as z:
            for name, data, executable in files:
                info = zipfile.ZipInfo(name, when)
                info.create_system = 3
                info.external_attr = (0o100755 if executable else 0o100644) << 16
                info.compress_type = zipfile.ZIP_DEFLATED
                z.writestr(info, data, compresslevel=9)
        os.replace(partial, whl)
    finally:
        if os.path.exists(partial):
            os.remove(partial)
    write_components(whl, kind, tag)
    return whl


def write_components(whl, kind, tag):
    """<wheel>.components.json for packaging/sbom.py: what the wheel
    needs from the machine it lands on"""
    depends = [{"name": "Python", "version": REQUIRES_PYTHON, "purl": "pkg:generic/python"}]
    components = []
    if kind == "pe":
        components.append({"name": "Microsoft Visual C++ runtime",
                           "supplier": "Organization: Microsoft",
                           "comment": "linked statically into ecuio.dll and the surrogates"})
    elif kind == "elf":
        glibc = re.match(r"manylinux_(\d+)_(\d+)_", tag)
        depends += [{"name": "glibc", "version": f">= {glibc.group(1)}.{glibc.group(2)}"},
                    {"name": "libstdc++"}, {"name": "libgcc_s"}]
    else:
        m = re.match(r"macosx_(\d+)_(\d+)_", tag)
        depends.append({"name": "macOS", "version": f">= {m.group(1)}.{m.group(2)}"})
    data = {"download": os.path.basename(whl), "purpose": "LIBRARY",
            "components": components, "depends": depends}
    with open(whl + ".components.json", "w", encoding="utf-8", newline="\n") as f:
        json.dump(data, f, indent=2, sort_keys=True)
        f.write("\n")


# ---- the self-test: made-up binaries and a made-up package

def make_pe(machine, imports, pe32_plus=True):
    """A minimal PE: headers, one section holding the import table"""
    pe, opt_size = 0x80, 240 if pe32_plus else 224
    sec_hdr = pe + 24 + opt_size
    raw, va = 0x400, 0x1000
    body = bytearray(0x200)
    # the descriptors, then the names
    names_at = 20 * (len(imports) + 1)
    blob = bytearray()
    for i, name in enumerate(imports):
        struct.pack_into("<IIIII", body, 20 * i, 0, 0, 0, va + names_at + len(blob), 0)
        blob += name.encode() + b"\0"
    body[names_at:names_at + len(blob)] = blob
    data = bytearray(raw + len(body))
    data[0:2] = b"MZ"
    struct.pack_into("<I", data, 0x3C, pe)
    data[pe:pe + 4] = b"PE\0\0"
    struct.pack_into("<HHIIIHH", data, pe + 4, machine, 1, 0, 0, 0, opt_size, 0)
    opt = pe + 24
    struct.pack_into("<H", data, opt, 0x20B if pe32_plus else 0x10B)
    dirs = opt + (112 if pe32_plus else 96)
    struct.pack_into("<II", data, dirs + 8, va if imports else 0, len(body))
    data[sec_hdr:sec_hdr + 8] = b".idata\0\0"
    struct.pack_into("<IIII", data, sec_hdr + 8, len(body), va, len(body), raw)
    data[raw:] = body
    return bytes(data)


def make_elf(machine, needed, soname, versions):
    """A minimal ELF64: .dynstr, .dynamic and .gnu.version_r"""
    strtab = bytearray(b"\0")

    def s(text):
        at = len(strtab)
        strtab.extend(text.encode() + b"\0")
        return at

    dyn = bytearray()
    for n in needed:
        dyn += struct.pack("<qQ", 1, s(n))
    if soname:
        dyn += struct.pack("<qQ", 14, s(soname))
    dyn += struct.pack("<qQ", 0, 0)
    vr = bytearray()
    if versions:
        vr += struct.pack("<HHIII", 1, len(versions), s("libc.so.6"), 16, 0)
        for i, v in enumerate(versions):
            last = i == len(versions) - 1
            vr += struct.pack("<IHHII", 0, 0, 0, s(v), 0 if last else 16)
    off = 64
    sections = [(0, 0, 0, 0)]                  # the null section
    blobs = bytearray()
    for sh_type, blob, link in ((3, strtab, 0), (6, dyn, 1), (0x6FFFFFFE, vr, 1)):
        sections.append((sh_type, off + len(blobs), len(blob), link))
        blobs += blob
    shoff = off + len(blobs)
    data = bytearray(shoff + 64 * len(sections))
    data[0:6] = b"\x7fELF\x02\x01"
    struct.pack_into("<H", data, 18, machine)
    struct.pack_into("<QHHH", data, 0x28, shoff, 0, 0, 0)
    struct.pack_into("<HHH", data, 0x3A, 64, len(sections), 0)
    data[off:off + len(blobs)] = blobs
    for i, (sh_type, o, size, link) in enumerate(sections):
        h = shoff + 64 * i
        struct.pack_into("<I", data, h + 4, sh_type)
        struct.pack_into("<QQ", data, h + 0x18, o, size)
        struct.pack_into("<I", data, h + 0x28, link)
    return bytes(data)


def make_macho(cpu, minos, dylibs, build_version=True):
    cmds = bytearray()
    v = (minos[0] << 16) | (minos[1] << 8)
    if build_version:
        cmds += struct.pack("<IIIIII", 0x32, 24, 1, v, v, 0)
    else:
        cmds += struct.pack("<IIII", 0x24, 16, v, v)
    for d in dylibs:
        name = d.encode() + b"\0"
        size = (24 + len(name) + 7) // 8 * 8
        cmds += struct.pack("<IIIIII", 0xC, size, 24, 0, 0, 0) + name.ljust(size - 24, b"\0")
    ncmds = 1 + len(dylibs)
    head = struct.pack("<IiiIIIII", 0xFEEDFACF, cpu, 0, 6, ncmds, len(cmds), 0, 0)
    return head + bytes(cmds)


def selftest():
    failures = []

    def check(cond, what):
        print(("ok    " if cond else "FAIL  ") + what)
        if not cond:
            failures.append(what)

    def refused(fn, words, what):
        try:
            fn()
        except Refused as e:
            check(words in str(e), f"{what}: {e}")
            return
        check(False, f"{what}: not refused")

    win = make_pe(0x8664, ["bcrypt.dll", "KERNEL32.dll"])
    check(pe_info(win) == (0x8664, ["bcrypt.dll", "KERNEL32.dll"]), "a PE's machine and imports")
    check(pe_info(make_pe(0x14C, ["KERNEL32.dll"], pe32_plus=False))[1] == ["KERNEL32.dll"],
          "a PE32's imports")
    check(auto_tag(win) == "win_amd64", "an x64 DLL is win_amd64")
    refused(lambda: check_library(make_pe(0x8664, ["MSVCP140.dll", "KERNEL32.dll"]), "auto"),
            "imports the C runtime (MSVCP140.dll)", "a DLL importing msvcp140")
    refused(lambda: check_library(make_pe(0x8664, ["api-ms-win-crt-heap-l1-1-0.dll"]), "auto"),
            "C runtime", "a DLL importing the universal CRT")
    refused(lambda: check_library(win, "win32"), "the library is win_amd64", "a DLL under the x86 tag")

    elf = make_elf(62, ["libstdc++.so.6", "libc.so.6"], "libecuio.so.0.6",
                   ["GLIBC_2.17", "GLIBC_2.39", "GLIBC_2.34"])
    check(elf_info(elf) == (62, ["libstdc++.so.6", "libc.so.6"], "libecuio.so.0.6", (2, 39)),
          "an ELF's machine, NEEDED, SONAME and newest glibc")
    check(auto_tag(elf) == "manylinux_2_39_x86_64", "the newest glibc names the tag")
    check(check_library(elf, "manylinux_2_39_x86_64") == "manylinux_2_39_x86_64",
          "a tag at the library's glibc")
    check(check_library(elf, "manylinux_2_40_x86_64") == "manylinux_2_40_x86_64",
          "a tag past the library's glibc")
    refused(lambda: check_library(elf, "manylinux_2_38_x86_64"), "needs GLIBC_2.39",
            "a tag older than the library's glibc")
    refused(lambda: check_library(elf, "manylinux_2_39_aarch64"), "the library is",
            "the other arch's tag")
    refused(lambda: check_library(make_elf(62, ["libQt6Core.so.6"], "libecuio.so.0.6", []), "auto"),
            "outside manylinux's set", "a library outside manylinux's set")
    refused(lambda: check_library(make_elf(62, ["libc.so.6"], "", []), "auto"), "no SONAME",
            "an ELF without a SONAME")
    check(auto_tag(make_elf(183, ["libc.so.6"], "x", [])) == "manylinux_2_17_aarch64",
          "an aarch64 library with no glibc version is manylinux_2_17")

    mac = make_macho(0x0100000C, (13, 0), ["/usr/lib/libc++.1.dylib", "/usr/lib/libSystem.B.dylib"])
    check(macho_info(mac) == (0x0100000C, (13, 0),
                              ["/usr/lib/libc++.1.dylib", "/usr/lib/libSystem.B.dylib"]),
          "a Mach-O's CPU, minos and dylibs")
    check(auto_tag(mac) == "macosx_13_0_arm64", "macOS 13.0 is macosx_13_0")
    check(auto_tag(make_macho(0x0100000C, (14, 4), [])) == "macosx_14_0_arm64",
          "macOS 14.4 is macosx_14_0: the major alone from 11")
    check(check_library(make_macho(0x0100000C, (14, 4), []), "macosx_14_0_arm64")
          == "macosx_14_0_arm64", "a 14.4 library takes the macosx_14_0 tag")
    refused(lambda: check_library(make_macho(0x0100000C, (13, 0), []), "macosx_14_4_arm64"),
            "is <major>_0", "a tag naming a minor from macOS 11")
    check(auto_tag(make_macho(0x01000007, (10, 15), [], build_version=False)) ==
          "macosx_10_15_x86_64", "an old minimum through LC_VERSION_MIN_MACOSX")
    refused(lambda: check_library(make_macho(0x0100000C, (14, 4), []), "macosx_13_0_arm64"),
            "needs macOS 14.4", "a tag older than the library's minos")
    refused(lambda: check_library(make_macho(0x0100000C, (13, 0), ["@rpath/libfoo.dylib"]), "auto"),
            "outside the system's", "a dylib outside the system's")

    check(pep440("0.6.0") == ("0.6.0", "0.6.0") and pep440("0.6.0-rc2") == ("0.6.0rc2", "0.6.0"),
          "a tag's version in PEP 440's form")
    refused(lambda: pep440("0.6"), "isn't X.Y.Z", "a version that isn't X.Y.Z")

    with tempfile.TemporaryDirectory() as d:
        pkg = os.path.join(d, "havne")
        os.makedirs(os.path.join(pkg, "__pycache__"))
        for name, text in (("__init__.py", "from ._version import __version__\n"),
                           ("_version.py", '__version__ = "0.6.0"\n'), ("notes.txt", "no\n"),
                           ("__pycache__/x.pyc", "no\n")):
            with open(os.path.join(pkg, name), "w", encoding="utf-8") as f:
                f.write(text)
        lic = os.path.join(d, "LICENSE")
        with open(lic, "w", encoding="utf-8") as f:
            f.write("MIT\n")
        so = os.path.join(d, "libecuio.so.0.6.0")
        with open(so, "wb") as f:
            f.write(elf)
        args = dict(package_dir=pkg, licenses=[(lic, "LICENSE")], epoch=1767225600,
                    version_of=lambda path: "0.6.0")
        a = build("0.6.0", so, os.path.join(d, "a"), **args)
        b = build("0.6.0", so, os.path.join(d, "b"), **args)
        with open(a, "rb") as fa, open(b, "rb") as fb:
            check(fa.read() == fb.read(), "two builds are the same bytes")
        check(os.path.basename(a) == "havne-0.6.0-py3-none-manylinux_2_39_x86_64.whl",
              "the wheel's name from the library's tag")
        with zipfile.ZipFile(a) as z:
            names = z.namelist()
            check(names == sorted(names[:-1]) + ["havne-0.6.0.dist-info/RECORD"],
                  "entries sorted, RECORD last")
            check("havne/libecuio.so.0.6" in names and "havne/notes.txt" not in names and
                  not any("__pycache__" in n for n in names),
                  "the library under its SONAME, the .py files only")
            rows = z.read("havne-0.6.0.dist-info/RECORD").decode().splitlines()
            ok = all(_record_hash(z.read(r.split(",")[0])) == r.split(",")[1]
                     for r in rows if not r.endswith(",,"))
            check(ok and len(rows) == len(names), "RECORD hashes every entry")
            check("Tag: py3-none-manylinux_2_39_x86_64" in z.read("havne-0.6.0.dist-info/WHEEL").decode(),
                  "WHEEL names the tag")
            info = z.getinfo("havne/libecuio.so.0.6")
            check(info.external_attr >> 16 == 0o100755 and info.date_time == (2026, 1, 1, 0, 0, 0),
                  "the library executable, every time the epoch's")
        with open(a + ".components.json", encoding="utf-8") as f:
            comp = json.load(f)
        check(comp["download"] == os.path.basename(a) and
              {"name": "glibc", "version": ">= 2.39"} in comp["depends"],
              "the component list names the wheel and its glibc")
        check(not [f for f in os.listdir(os.path.join(d, "a")) if f.endswith(".partial")],
              "no partial left")
        rc = build("0.6.0-rc1", so, os.path.join(d, "rc"), **args)
        check(os.path.basename(rc).startswith("havne-0.6.0rc1-"), "an rc's wheel is 0.6.0rc1")
        refused(lambda: build("0.7.0", so, os.path.join(d, "c"), **args),
                "_version.py says 0.6.0", "a _version.py of another version")
        refused(lambda: build("0.6.0", so, os.path.join(d, "c"),
                              **{**args, "version_of": lambda p: "0.5.1"}),
                "the library is core 0.5.1", "a library of another version")
        dll = os.path.join(d, "ecuio.dll")
        with open(dll, "wb") as f:
            f.write(win)
        sur86, sur64 = os.path.join(d, "s86.exe"), os.path.join(d, "s64.exe")
        with open(sur86, "wb") as f:
            f.write(make_pe(0x14C, ["KERNEL32.dll"], pe32_plus=False))
        with open(sur64, "wb") as f:
            f.write(make_pe(0x8664, ["KERNEL32.dll"]))
        w = build("0.6.0", dll, os.path.join(d, "w"), surrogates={"x86": sur86, "x64": sur64}, **args)
        with zipfile.ZipFile(w) as z:
            check({"havne/ecuio.dll", "havne/ecuio-j2534-surrogate.exe",
                   "havne/ecuio-j2534-surrogate-x64.exe"} <= set(z.namelist()),
                  "a Windows wheel holds the DLL and both surrogates by their names")
        refused(lambda: build("0.6.0", dll, os.path.join(d, "c"), surrogates={"x64": sur64}, **args),
                "needs the x86 surrogate", "a Windows wheel without the x86 surrogate")
        refused(lambda: build("0.6.0", dll, os.path.join(d, "c"),
                              surrogates={"x86": sur64, "x64": sur64}, **args),
                "not 0x14c", "the x64 surrogate in the x86's place")
        bad = os.path.join(d, "bad.exe")
        with open(bad, "wb") as f:
            f.write(make_pe(0x8664, ["VCRUNTIME140.dll"]))
        refused(lambda: build("0.6.0", dll, os.path.join(d, "c"),
                              surrogates={"x86": sur86, "x64": bad}, **args),
                "imports the C runtime", "a surrogate importing the C runtime")
        refused(lambda: build("0.6.0", so, os.path.join(d, "c"),
                              surrogates={"x86": sur86, "x64": sur64}, **args),
                "only in a Windows wheel", "surrogates in a Linux wheel")

    print(f"{'FAIL' if failures else 'ok'}: {len(failures)} failed")
    return 1 if failures else 0


def main(argv):
    p = argparse.ArgumentParser(description="Build the havne wheel")
    sub = p.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("build")
    b.add_argument("--version", required=True, help="X.Y.Z or X.Y.Z-rcN (the tag's)")
    b.add_argument("--library", required=True, help="the shared core")
    b.add_argument("--platform-tag", default="auto")
    b.add_argument("--surrogate-x86")
    b.add_argument("--surrogate-x64")
    b.add_argument("--out", required=True)
    sub.add_parser("selftest")
    args = p.parse_args(argv)
    if args.cmd == "selftest":
        return selftest()
    surrogates = {k: v for k, v in (("x86", args.surrogate_x86), ("x64", args.surrogate_x64)) if v}
    try:
        whl = build(args.version, args.library, args.out, args.platform_tag, surrogates or None)
    except (Refused, OSError) as e:
        print(f"make_wheel: {e}", file=sys.stderr)
        return 2
    print(f"make_wheel: {whl}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
