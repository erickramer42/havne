# File/layer: tests/example_notebook.py | Python 3.11+, ctest's ExampleTest.notebook
# Testing: HARNESS, ExampleTest.notebook
#
# Runs python/examples/ecu_states.ipynb top to bottom, as a reader runs
# it, on this tree's package and library: every cell must run, and its
# last cell's own check must hold. Exit 77 (ctest's skip) without
# scikit-learn, nbconvert or ipykernel: CI's jobs carry no scientific
# stack, so it runs locally (build-py-venv).
#
# The kernel is this Python, through a kernelspec of the test's own in a
# temporary folder: a user's "python3" kernelspec may name another
# interpreter, one without the package's needs.

import importlib.util
import json
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NOTEBOOK = os.path.join(ROOT, "python", "examples", "ecu_states.ipynb")
NEEDS = ("sklearn", "numpy", "nbconvert", "nbclient", "ipykernel")
KERNEL = "havne-example-test"


def main():
    missing = [m for m in NEEDS if importlib.util.find_spec(m) is None]
    if missing:
        print(f"skipped: no {', '.join(missing)} in {sys.executable}")
        return 77
    with tempfile.TemporaryDirectory(prefix="havne-notebook-") as tmp:
        spec = os.path.join(tmp, "kernels", KERNEL)
        os.makedirs(spec)
        with open(os.path.join(spec, "kernel.json"), "w", encoding="utf-8") as f:
            json.dump({"argv": [sys.executable, "-m", "ipykernel_launcher", "-f",
                                "{connection_file}"],
                       "display_name": KERNEL, "language": "python"}, f)
        env = {**os.environ, "JUPYTER_PATH": tmp,
               "PYTHONPATH": os.path.join(ROOT, "python"),
               "PYTHONDONTWRITEBYTECODE": "1", "PYTHONIOENCODING": "utf-8"}
        run = subprocess.run([sys.executable, "-m", "nbconvert", "--to", "notebook", "--execute",
                              f"--ExecutePreprocessor.kernel_name={KERNEL}",
                              "--ExecutePreprocessor.timeout=120",
                              "--output-dir", tmp, NOTEBOOK],
                             capture_output=True, text=True, encoding="utf-8",
                             errors="replace", env=env, timeout=300)
        if run.returncode != 0:
            print(run.stdout)
            print(run.stderr)
            print(f"FAIL: the notebook stopped (exit {run.returncode})")
            return 1
    print("the notebook ran to its end, its own check held")
    return 0


if __name__ == "__main__":
    sys.exit(main())
