import importlib.resources
import json

import havne


def load(path=None):
    """The wanted list: the package's, or the file at path"""
    if path is None:
        text = importlib.resources.files(havne).joinpath("data/wanted-list.json").read_text("utf-8")
    else:
        with open(path, encoding="utf-8") as f:
            text = f.read()
    return json.loads(text)


def signals(doc, packs=None):
    return [s for p in doc["packs"] if packs is None or p["key"] in packs for s in p["signals"]]


def examples(signal, bits, signed=None):
    return [e for e in signal["encodings"]
            if e["bits"] == bits and (signed is None or e["signed"] == signed)]
