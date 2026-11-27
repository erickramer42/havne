import ctypes
import hashlib
import json
import os
import subprocess
import sys
import tempfile
import threading


def run(cli, args, env=None):
    p = subprocess.run([cli] + args, capture_output=True, text=True, env=env,
                       stdin=subprocess.DEVNULL, timeout=120)
    return p.returncode, p.stdout, p.stderr


def rows_of(text):
    return [json.loads(line) for line in text.splitlines() if line.strip()]


def of_type(rows, kind):
    return [r for r in rows if r.get("type") == kind]


def fingerprint_of(cli, package):
    code, out, err = run(cli, ["plugins", "terms", package, "--json"])
    if code != 0:
        raise SystemExit(f"FAIL  plugins terms: exit {code} {err!r}")
    return next(r["fingerprint"] for r in rows_of(out)
                if r.get("type") == "terms")


def consent(cli, package, store):
    code, _, err = run(cli, ["plugins", "consent", package, "--consent-store",
                             store, "--give", fingerprint_of(cli, package)])
    if code != 0:
        raise SystemExit(f"FAIL  --give: exit {code} {err!r}")


class Sim:
    def __init__(self, cli, uri):
        self.p = subprocess.Popen(
            [cli, "sim", uri, "--elm327", "tcp:0", "--seconds", "120"],
            stdout=subprocess.PIPE, text=True)
        self.port = None
        for line in self.p.stdout:
            j = json.loads(line)
            if j["type"] == "elm327":
                self.port = int(j["listen"].rsplit(":", 1)[1])
                break
        if self.port is None:
            raise SystemExit("FAIL  the emulator wrote no elm327 line")
        threading.Thread(target=self.p.stdout.read, daemon=True).start()

    def end(self):
        self.p.terminate()
        self.p.wait(timeout=60)


def elm_uri(port):
    return (f"elm327://127.0.0.1:{port}?profile=plugin.talker.base"
            "&identify=0&diagnose=0&probe=0")


def dump_args(uri, package, d, extra=(), out="out"):
    return (["dump", uri, "--plugin", package, "--out", os.path.join(d, out),
             "--consent-store", os.path.join(d, "consent.json"),
             "--security-store", os.path.join(d, "security.json"),
             "--vehicle", "bench"] + list(extra))


def kept(d):
    folder = os.path.join(d, "out")
    return sorted(os.listdir(folder)) if os.path.isdir(folder) else []


def read_back(lib_path, path):
    with open(path, "rb") as f:
        whole = f.read()
    name = os.path.basename(path)
    if not name.endswith(".havnedump") or \
            hashlib.sha256(whole).hexdigest() != name[-74:-10]:
        return f"the name doesn't hold the file's checksum: {name}"
    if lib_path == "-":
        return None
    lib = ctypes.CDLL(lib_path)
    lib.ecuio_dump_open.restype = ctypes.c_int
    lib.ecuio_dump_open.argtypes = [ctypes.c_char_p,
                                    ctypes.POINTER(ctypes.c_void_p)]
    lib.ecuio_dump_verify.restype = ctypes.c_int
    lib.ecuio_dump_verify.argtypes = [ctypes.c_void_p, ctypes.c_void_p,
                                      ctypes.c_void_p, ctypes.c_char_p,
                                      ctypes.c_size_t]
    lib.ecuio_dump_close.argtypes = [ctypes.c_void_p]
    handle = ctypes.c_void_p()
    if lib.ecuio_dump_open(path.encode("utf-8"), ctypes.byref(handle)) != 0:
        return "ecuio_dump_open refused the copy"
    out = ctypes.create_string_buffer(256)
    st = lib.ecuio_dump_verify(handle, None, None, out, 256)
    lib.ecuio_dump_close(handle)
    if st != 0 or out.value.decode() != name:
        return f"ecuio_dump_verify: {st} {out.value!r}"
    return None


def saved_case(cli, plain, lib_path, d):
    sim = Sim(cli, "mock://sim?values=parked")
    try:
        code, out, err = run(cli, dump_args(elm_uri(sim.port), plain, d,
                                            ["--parked"]))
    finally:
        sim.end()
    rows = rows_of(out)
    dumps = of_type(rows, "dump")
    if code != 0 or len(dumps) != 1 or dumps[0].get("end") != "saved" \
            or dumps[0].get("verified") is not True \
            or rows[-1] != {"type": "end", "reason": "saved"}:
        return f"saved: exit {code} {dumps!r} {err!r}"
    if [r.get("id") for r in of_type(rows, "operation")] != ["copy"]:
        return f"saved: the plan's line {of_type(rows, 'operation')!r}"
    phases = [p["phase"] for p in of_type(rows, "phase")]
    for want in ("reading", "comparing", "done"):
        if want not in phases:
            return f"saved: no {want} phase in {phases!r}"
    if kept(d) != [dumps[0]["name"]] or dumps[0]["sha256"] not in \
            dumps[0]["name"] or dumps[0]["bytes"] != 2048:
        return f"saved: the folder holds {kept(d)!r} for {dumps[0]!r}"
    return read_back(lib_path, dumps[0]["path"])


def refusals_case(cli, plain, kernel, d):
    sim = Sim(cli, "mock://sim?values=canned")
    try:
        code, out, err = run(cli, dump_args(elm_uri(sim.port), plain, d))
        if code != 2 or out or "--parked" not in err:
            return f"not parked: exit {code} {out!r} {err!r}"
        code, out, err = run(cli, dump_args(elm_uri(sim.port), plain, d,
                                            ["--parked"]))
        if code != 4 or of_type(rows_of(out), "dump")[0].get("end") != \
                "engine_running" or kept(d):
            return f"engine running: exit {code} {out!r} {err!r}"
        code, out, err = run(cli, dump_args(elm_uri(sim.port), kernel, d,
                                            ["--parked"]))
        errors = of_type(rows_of(out), "error")
        if code != 3 or not errors or "J2534" not in err:
            return f"a kernel read on ELM327: exit {code} {out!r} {err!r}"
    finally:
        sim.end()
    return None


def cancel_run(cli, plain, d, at, out):
    sim = Sim(cli, "mock://sim?values=parked")
    env = dict(os.environ, HAVNE_CLI_DUMP_CANCEL_AT=at)
    try:
        code, text, err = run(cli, dump_args(elm_uri(sim.port), plain, d,
                                             ["--parked"], out), env)
    finally:
        sim.end()
    return code, rows_of(text), err


def abort_case(cli, plain, d):
    code, rows, err = cancel_run(cli, plain, d, "run", "out")
    dumps = of_type(rows, "dump")
    if code != 4 or len(dumps) != 1 or dumps[0].get("end") != "canceled"             or kept(d) or "stopping" not in err:
        return f"abort: exit {code} {dumps!r} {kept(d)!r} {err!r}"
    return None


def before_case(cli, plain, d):
    code, rows, err = cancel_run(cli, plain, d, "start", "before")
    dumps = of_type(rows, "dump")
    if code != 4 or [x.get("end") for x in dumps] != ["canceled"]             or of_type(rows, "phase") or "nothing was asked" not in err             or os.path.exists(os.path.join(d, "before")):
        return f"before the start: exit {code} {rows!r} {err!r}"
    return None


def fake_env(dll_knobs):
    env = {k: v for k, v in os.environ.items()
           if not k.startswith("ECUIO_FAKE_")}
    env.update(dll_knobs)
    return env


def kernel_case(cli, kernel, dll, lib_path, d):
    uri = f"j2534://{dll}?profile=plugin.talker.base&identify=0&diagnose=0"
    knobs = {"ECUIO_FAKE_ENGINE_OFF": "1", "ECUIO_FAKE_KERNEL": "1",
             "ECUIO_FAKE_VBATT": "11700"}
    code, out, err = run(cli, dump_args(uri, kernel, d, ["--parked"]),
                         fake_env(knobs))
    rows = rows_of(out)
    dumps = of_type(rows, "dump")
    warned = [w["check"] for w in of_type(rows, "warning")]
    if code != 0 or len(dumps) != 1 or dumps[0].get("end") != "saved" \
            or dumps[0].get("bytes") != 4096 or warned.count("volts_low") != 1 \
            or "11.7 V" not in err:
        return f"kernel: exit {code} {dumps!r} {warned!r} {err!r}"
    why = read_back(lib_path, dumps[0]["path"])
    if why:
        return why
    knobs = {"ECUIO_FAKE_ENGINE_OFF": "1", "ECUIO_FAKE_KERNEL": "1",
             "ECUIO_FAKE_VBATT_SAG": "10900:3"}
    code, out, err = run(cli, dump_args(uri, kernel, d, ["--parked"], "sag"),
                         fake_env(knobs))
    rows = rows_of(out)
    warned = [w["check"] for w in of_type(rows, "warning")]
    if code != 0 or warned.count("volts_sag") != 1 or \
            "the car may not start" not in err:
        return f"kernel sag: exit {code} {warned!r} {err!r}"
    return None


def fixtures(writer, d):
    plain = os.path.join(d, "plain.json")
    kernel = os.path.join(d, "kernel.json")
    done = subprocess.run([writer, plain, kernel], capture_output=True)
    if done.returncode != 0:
        raise SystemExit(f"FAIL  the fixture writer: {done.stderr!r}")
    return plain, kernel


def main(argv):
    if len(argv) < 4:
        print("usage: cli_dump.py saved|refusals|abort|before|kernel <havne-cli> "
              "<cli_dump_fixture> [<shared core>|-] [<fake_j2534.dll>]")
        return 2
    case, cli, writer = argv[1], argv[2], argv[3]
    lib_path = argv[4] if len(argv) > 4 else "-"
    with tempfile.TemporaryDirectory(prefix="havne-cli-dump-") as d:
        plain, kernel = fixtures(writer, d)
        for package in (plain, kernel):
            consent(cli, package, os.path.join(d, "consent.json"))
        if case == "saved":
            why = saved_case(cli, plain, lib_path, d)
        elif case == "refusals":
            store = os.path.join(d, "none.json")
            code, out, err = run(cli, ["dump", "elm327://127.0.0.1:1"
                                       "?profile=plugin.talker.base", "--plugin",
                                       plain, "--out", d, "--consent-store",
                                       store, "--parked"])
            why = None if code == 2 and not out and "consent" in err else \
                f"no consent: exit {code} {out!r} {err!r}"
            why = why or refusals_case(cli, plain, kernel, d)
        elif case == "abort":
            why = abort_case(cli, plain, d)
        elif case == "before":
            why = before_case(cli, plain, d)
        elif case == "kernel" and len(argv) > 5:
            why = kernel_case(cli, kernel, argv[5], lib_path, d)
        else:
            print(f"FAIL  unknown case {case}")
            return 2
    if why:
        print(f"FAIL  {why}")
        return 1
    print(f"ok    {case}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
