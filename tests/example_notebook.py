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
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NOTEBOOK = os.path.join(ROOT, "python", "examples", "ecu_states.ipynb")
NEEDS = ("sklearn", "numpy", "nbconvert", "nbclient", "ipykernel")
KERNEL = "havne-example-test"
# The run's third-party modules, SciPy's files among them: read cold from a
# disk other builds keep busy, their import alone has taken 160 s
WARM = ("import nbconvert, nbclient, ipykernel.kernelapp, numpy, sklearn.linear_model, "
        "sklearn.model_selection, sklearn.pipeline, sklearn.preprocessing, sklearn.tree")
WARM_SECONDS = 600
RUN_SECONDS = 240  # the two under ctest's TIMEOUT of 900, so the words are this script's


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
        # The imports first, bounded apart: a slow disk then never reads as
        # a hung notebook, and the kernel finds the files cached
        started = time.monotonic()
        try:
            warm = subprocess.run([sys.executable, "-B", "-c", WARM], capture_output=True,
                                  text=True, encoding="utf-8", errors="replace", env=env,
                                  timeout=WARM_SECONDS)
        except subprocess.TimeoutExpired:
            print(f"FAIL: the run's modules didn't import within {WARM_SECONDS} s")
            return 1
        if warm.returncode != 0:
            print(warm.stderr)
            print(f"FAIL: the run's modules didn't import (exit {warm.returncode})")
            return 1
        print(f"the run's modules imported in {time.monotonic() - started:.1f} s")
        # One bound for the notebook: the test measures that it runs to its
        # end and its own check holds, nothing about time
        started = time.monotonic()
        try:
            run = subprocess.run([sys.executable, "-m", "nbconvert", "--to", "notebook",
                                  "--execute",
                                  f"--ExecutePreprocessor.kernel_name={KERNEL}",
                                  "--ExecutePreprocessor.timeout=-1",
                                  "--output-dir", tmp, NOTEBOOK],
                                 capture_output=True, text=True, encoding="utf-8",
                                 errors="replace", env=env, timeout=RUN_SECONDS)
        except subprocess.TimeoutExpired:
            print(f"FAIL: the notebook didn't finish within {RUN_SECONDS} s")
            return 1
        if run.returncode != 0:
            print(run.stdout)
            print(run.stderr)
            print(f"FAIL: the notebook stopped (exit {run.returncode})")
            return 1
    print(f"the notebook ran to its end in {time.monotonic() - started:.1f} s, "
          "its own check held")
    return 0


if __name__ == "__main__":
    sys.exit(main())
