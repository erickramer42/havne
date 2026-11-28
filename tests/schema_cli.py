import json
import os
import subprocess


class Cli:
    def __init__(self, exe, scratch):
        self.exe = exe
        self.scratch = scratch
        self.count = 0

    def check(self, kind, doc=None, path=None):
        """(accepted, the refusal's detail) from signals or plugins check"""
        if path is None:
            self.count += 1
            path = os.path.join(self.scratch, f"case{self.count}.json")
            with open(path, "w", encoding="utf-8", newline="\n") as f:
                json.dump(doc, f, ensure_ascii=False, indent=1)
        word = {"signal-file": "signals", "condition-file": "conditions"}.get(kind, "plugins")
        r = subprocess.run([self.exe, word, "check", path], capture_output=True, timeout=30)
        detail = ""
        for line in r.stdout.decode("utf-8", errors="replace").splitlines():
            try:
                record = json.loads(line)
            except ValueError:
                continue
            if record.get("type") == "error":
                detail = record.get("detail", "")
        if r.returncode not in (0, 3):
            raise RuntimeError(f"havne-cli {word} check exited {r.returncode}: "
                               f"{r.stderr.decode(errors='replace')}")
        return r.returncode == 0, detail
