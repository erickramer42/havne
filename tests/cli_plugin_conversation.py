# File/layer: tests/cli_plugin_conversation.py | test-only, Python 3.8+
# Testing: HARNESS (ctest CliPluginConversationTest.AFolderReadsAsItsPackedForm):
#          havne-cli as a child, on a folder this test writes
#
#   python3 tests/cli_plugin_conversation.py <havne-cli> <packed plugin>
#
# A plugin with the conversation tier (ADR-0066, "havne_plugin" 2) through
# havne-cli in both forms. The packed fixture is unpacked into a folder
# (its manifest without "files", each data file as JSON, each binary file
# as its bytes), and:
# - plugins check writes the same conversation line for both, after the
#   plugin line, with the module's size and SHA-256;
# - each security level's "ecu" is its manifest id, one of the line's ecus;
# - plugins list writes it too;
# - a folder whose module lost a byte is refused at the hash, exit 3;
# - plugins put and plugins drop refuse the packed file, exit 3, and
#   write no output file;
# - a plugin at every bound (32 ECUs with ids of 31 characters, 14
#   sub-functions, 32 security levels at their largest numbers) still
#   gets its line whole.

import base64
import hashlib
import json
import os
import subprocess
import sys
import tempfile


def run(cli, *args):
    r = subprocess.run([cli] + list(args), capture_output=True, timeout=60)
    lines = []
    for line in r.stdout.decode("utf-8", errors="replace").splitlines():
        if line.strip():
            lines.append(json.loads(line))
    return r.returncode, lines


def unpack(packed, folder):
    """The packed plugin as a folder; the module's path in it"""
    manifest = {k: v for k, v in packed.items() if k != "files"}
    module = None
    for path, entry in packed["files"].items():
        full = os.path.join(folder, *path.split("/"))
        os.makedirs(os.path.dirname(full), exist_ok=True)
        if isinstance(entry, dict) and "kind" in entry:
            with open(full, "wb") as f:
                f.write(base64.b64decode(entry["base64"]))
            module = full
        else:
            with open(full, "w", encoding="utf-8", newline="\n") as f:
                json.dump(entry, f, indent=1)
    with open(os.path.join(folder, "manifest.json"), "w", encoding="utf-8", newline="\n") as f:
        json.dump(manifest, f, indent=1)
    return module


def main():
    if len(sys.argv) != 3:
        print("usage: cli_plugin_conversation.py <havne-cli> <packed plugin>")
        return 2
    cli, fixture = sys.argv[1], sys.argv[2]
    with open(fixture, encoding="utf-8") as f:
        packed = json.load(f)
    failures = []

    def check(ok, what):
        if not ok:
            failures.append(what)

    with tempfile.TemporaryDirectory(prefix="havne-conversation-") as scratch:
        folder = os.path.join(scratch, "talker")
        os.makedirs(folder)
        module = unpack(packed, folder)
        with open(module, "rb") as f:
            code = f.read()

        code_p, lines_p = run(cli, "plugins", "check", fixture)
        code_f, lines_f = run(cli, "plugins", "check", folder)
        check(code_p == 0 and code_f == 0, f"plugins check exits {code_p} packed, {code_f} folder")
        conv_p = [l for l in lines_p if l.get("type") == "conversation"]
        conv_f = [l for l in lines_f if l.get("type") == "conversation"]
        check(len(conv_p) == 1 and conv_p == conv_f,
              f"the conversation lines differ: packed {conv_p}, folder {conv_f}")
        if conv_p:
            line = conv_p[0]
            check(line["ecus"][0] == {"id": "pcm", "rx": "0x7E8", "tx": "0x7E0"},
                  f"an ECU by its id and both IDs: {line['ecus']}")
            check([s["ecu"] for s in line["security"]]
                  == [s["ecu"] for s in packed["conversation"]["security"]],
                  f"each level's ecu is the manifest's id: {line['security']}")
            check({s["ecu"] for s in line["security"]} <= {e["id"] for e in line["ecus"]},
                  f"each level's ecu is one of the line's ecus: {line['security']}")
            check(line["code"] == {"bytes": len(code), "sha256": hashlib.sha256(code).hexdigest()},
                  f"the code's size and hash: {line['code']}")
            types = [l.get("type") for l in lines_p]
            check(types.index("conversation") == types.index("plugin") + 1,
                  f"the conversation line follows the plugin line: {types}")
        plugin = [l for l in lines_p if l.get("type") == "plugin"]
        check(len(plugin) == 1 and plugin[0].get("tiers") == ["data", "conversation"],
              f"the plugin line's tiers: {plugin}")

        code_l, lines_l = run(cli, "plugins", "list", folder, fixture)
        listed = [l for l in lines_l if l.get("type") == "conversation"]
        check(code_l == 0 and [l.get("package") for l in listed] == [0, 1],
              f"plugins list: exit {code_l}, conversation lines {listed}")

        # A module that isn't the one the manifest names
        with open(module, "wb") as f:
            f.write(code[:-1])
        code_b, lines_b = run(cli, "plugins", "check", folder)
        errors = [l for l in lines_b if l.get("type") == "error"]
        check(code_b == 3 and len(errors) == 1 and "has SHA-256" in errors[0].get("detail", "")
              and errors[0]["detail"].startswith("manifest.json, line "),
              f"a changed module: exit {code_b}, {errors}")

        # Neither a put nor a drop carries a module over
        signals = os.path.join(scratch, "extra.json")
        with open(signals, "w", encoding="utf-8", newline="\n") as f:
            json.dump({"havne_signals": 1, "name": "Extra", "source": {"name": "t"},
                       "ecus": [{"id": "pcm", "rx": "7E8"}],
                       "signals": [{"key": "oil2", "name": "Oil 2", "ecu": "pcm",
                                    "read": {"service": "22", "number": "F40E"},
                                    "field": {"byte": 0}}]}, f)
        out = os.path.join(scratch, "out.json")
        for args in (["plugins", "put", fixture, signals, "--out", out],
                     ["plugins", "drop", fixture, "--variant", "base", "--out", out]):
            code_w, lines_w = run(cli, *args)
            errors = [l for l in lines_w if l.get("type") == "error"]
            check(code_w == 3 and len(errors) == 1
                  and "the conversation tier" in errors[0].get("detail", ""),
                  f"{' '.join(args[:2])}: exit {code_w}, {errors}")
            check(not os.path.exists(out), f"{' '.join(args[:2])} wrote a file")

        # The widest declaration plugin.h takes: its line is written whole
        wide = json.loads(json.dumps(packed))
        wide["ecus"] = [{"id": ("e%d" % n).ljust(31, "x"), "rx": "%X" % (0x700 + n),
                         "tx": "%X" % (0x600 + n)}
                        for n in range(32)]
        wide["files"]["data/base.json"]["signals"][0]["ecu"] = wide["ecus"][0]["id"]
        subs = ["%02X" % n for n in range(1, 15)]
        wide["conversation"]["ecus"] = [e["id"] for e in wide["ecus"]]
        wide["conversation"]["services"] = [
            {"sid": "10", "sub": ["03"]}, {"sid": "19"}, {"sid": "22"}, {"sid": "2A"},
            {"sid": "27", "sub": subs}, {"sid": "2C", "sub": ["01", "03"]},
            {"sid": "3E", "sub": ["00"]}]
        wide["conversation"]["security"] = [
            {"ecu": wide["ecus"][n]["id"], "level": subs[2 * (n % 7)], "seed_bytes": 255,
             "key_bytes": 58,
             "delay_ms": 3600000, "key_window_ms": 4000} for n in range(32)]
        widest = os.path.join(scratch, "widest.json")
        with open(widest, "w", encoding="utf-8", newline="\n") as f:
            json.dump(wide, f)
        code_x, lines_x = run(cli, "plugins", "check", widest)
        line = [l for l in lines_x if l.get("type") == "conversation"]
        check(code_x == 0 and len(line) == 1 and len(line[0]["ecus"]) == 32
              and len(line[0]["security"]) == 32 and len(line[0]["services"]) == 7
              and line[0]["security"][31]["delay_ms"] == 3600000
              and [s["ecu"] for s in line[0]["security"]] == [e["id"] for e in wide["ecus"]],
              f"the widest plugin: exit {code_x}, {[l for l in lines_x if l.get('type') == 'error']}"
              f", {len(line)} conversation lines")

    for f in failures:
        print("FAIL  " + f)
    if failures:
        return 1
    print("ok  packed and folder read alike; the hash, put and drop refuse; the widest fits")
    return 0


if __name__ == "__main__":
    sys.exit(main())
