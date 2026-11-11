# File/layer: python/tests/test_plugin.py | pytest
# Testing: HARNESS, PythonTest.test_plugin: the example plugin through the
#          package (ADR-0052)
#
# plugins/example-sim loaded from its folder (core reads each file
# through the package's callback, on the loading thread), its variants
# listed, one registered, a session decoding through it, and its
# recording holding the variant's file as signals.json. Two threads
# loading at once each read their own folder; a read that raises is a
# file core can't read; a file is read no further than core's bound. A
# packed plugin (one file, no read callback) loads too. A folder plugin's
# variants each give their own signal's gauge. A plugin names the core it
# requires.

import json
import os
import shutil
import threading
import time

import pytest

import havne
import havne.signals
from havne._core import lib

from conftest import PLUGINS, ROOT


def test_the_example_plugin_loads_and_a_session_decodes_through_a_variant():
    p = havne.Plugin(os.path.join(PLUGINS, "example-sim"))
    assert p.id == "example-sim"
    assert p.variants[0] == "base" and len(p.variants) >= 2
    assert p.register("base") == "plugin.example-sim.base"
    with havne.Session("mock://py", plugin=p, variant=p.variants[1]) as s:
        assert "profile=plugin.example-sim." + p.variants[1] in s.uri
        assert s.signals, "the variant's signals"


def test_a_plugin_session_s_recording_holds_its_variant_s_file(tmp_path):
    """Recorded as havne-cli stream --record --plugin records it: the
    variant's file goes in as signals.json, named in events.log with
    the plugin, so a replay decodes through it again"""
    p = havne.Plugin(os.path.join(PLUGINS, "example-sim"))
    variant = p.variants[1]
    with havne.Session("mock://py", plugin=p, variant=variant) as s:
        rec = s.record(tmp_path)
        s.start()
        time.sleep(0.3)
        folder = rec.folder
    assert os.path.isfile(os.path.join(folder, "signals.json"))
    with open(os.path.join(folder, "events.log"), encoding="utf-8") as f:
        header = f.read().split("\n\n")[0].splitlines()
    assert "profile  plugin.example-sim." + variant in header
    assert any(line.startswith("signal file  signals.json,") for line in header), header
    assert any(line.startswith("plugin  ") for line in header), header


def test_a_packed_plugin_loads_with_no_read_callback():
    p = havne.Plugin(os.path.join(ROOT, "tests", "fixtures", "plugins", "packed.json"))
    assert p.id == "packed"
    assert p.variants == ["base"]
    assert oil_name(p) == "Oil Temp"


def test_a_plugin_names_the_core_it_requires():
    """requires.core as (major, minor, patch): the packed fixture's
    manifest says 0.1.0, the example's is read from its own manifest"""
    packed = havne.Plugin(os.path.join(ROOT, "tests", "fixtures", "plugins", "packed.json"))
    assert packed.requires_core == (0, 1, 0)
    folder = os.path.join(PLUGINS, "example-sim")
    with open(os.path.join(folder, "manifest.json"), encoding="utf-8") as f:
        said = json.load(f)["requires"]["core"]
    assert havne.Plugin(folder).requires_core == tuple(int(n) for n in said.split("."))


def copy_of_the_example(folder, plugin_id, oil_name):
    """The example plugin under another id, its oil temperature named
    oil_name, so a load that read another folder's files shows"""
    shutil.copytree(os.path.join(PLUGINS, "example-sim"), folder)
    for name, old, new in (("manifest.json", '"id": "example-sim"', f'"id": "{plugin_id}"'),
                           ("data/base.json", '"name": "Oil Temp"', f'"name": "{oil_name}"')):
        path = os.path.join(folder, name)
        with open(path, encoding="utf-8") as f:
            body = f.read()
        assert body.count(old) == 1, name
        with open(path, "w", encoding="utf-8") as f:
            f.write(body.replace(old, new))
    return folder


def oil_name(plugin):
    d = lib.ecuio_plugin_variant_find(plugin._handle, 0, b"oil_temp")
    return d.contents.name.decode("utf-8")


def test_two_threads_load_two_folders_each_its_own(tmp_path):
    # core calls the read callback on each loading thread: each load
    # reads its own folder's files, whatever the other thread loads
    folders = {"copy-a": "Oil Temp A", "copy-b": "Oil Temp B"}
    paths = {i: copy_of_the_example(str(tmp_path / i), i, n) for i, n in folders.items()}
    start = threading.Barrier(len(paths))
    wrong = []

    def load(plugin_id):
        start.wait()
        for _ in range(50):
            p = havne.Plugin(paths[plugin_id])
            if (p.id, oil_name(p)) != (plugin_id, folders[plugin_id]):
                wrong.append((plugin_id, p.id, oil_name(p)))

    threads = [threading.Thread(target=load, args=(i,)) for i in paths]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    assert not wrong, wrong[:3]


def test_a_read_that_raises_is_a_file_that_cannot_be_read(tmp_path, monkeypatch):
    # Not an OSError: the callback still answers core, which refuses
    folder = copy_of_the_example(str(tmp_path / "raises"), "raises", "Oil Temp")
    real_open = open

    def raising_open(path, *args, **kwargs):
        if os.path.basename(path) != "manifest.json":
            raise RuntimeError("not a file system error")
        return real_open(path, *args, **kwargs)

    monkeypatch.setattr(havne.signals, "open", raising_open, raising=False)
    with pytest.raises(havne.Error) as e:
        havne.Plugin(folder)
    assert e.value.name == "INVALID_ARGUMENT"
    assert "data/base.json" in e.value.detail, e.value.detail


def test_a_data_file_is_read_no_further_than_core_takes(tmp_path, monkeypatch):
    # A file past core's bound is refused by core either way: the
    # package reads one byte past it, never the whole file
    folder = copy_of_the_example(str(tmp_path / "big"), "big", "Oil Temp")
    with open(os.path.join(folder, "data", "base.json"), "ab") as f:
        f.write(b" " * (2 * havne.c.ECUIO_SIGNAL_FILE_MAX_BYTES))
    real_open, asked = open, []

    class Recorded:
        def __init__(self, f):
            self.f = f

        def __enter__(self):
            return self

        def __exit__(self, *exc):
            self.f.close()

        def read(self, size=-1):
            asked.append(size)
            return self.f.read(size)

    def recording_open(path, *args, **kwargs):
        f = real_open(path, *args, **kwargs)
        return f if os.path.basename(path) == "manifest.json" else Recorded(f)

    monkeypatch.setattr(havne.signals, "open", recording_open, raising=False)
    with pytest.raises(havne.Error) as e:
        havne.Plugin(folder)
    assert e.value.name == "INVALID_ARGUMENT"
    assert asked == [havne.c.ECUIO_SIGNAL_FILE_MAX_BYTES + 1], asked


def test_a_folder_with_no_plugin_is_refused(tmp_path):
    with pytest.raises(OSError):
        havne.Plugin(str(tmp_path))
    (tmp_path / "manifest.json").write_text('{"havne_plugin": 1}', encoding="utf-8")
    with pytest.raises(havne.Error) as e:
        havne.Plugin(str(tmp_path))
    assert e.value.name == "INVALID_ARGUMENT"
    assert "manifest.json" in e.value.detail


def test_a_plugin_says_a_variant_s_gauge(tmp_path):
    signal = ('{"key": "oil", "name": "Oil", "read": {"service": "22", "number": "F40D"}, '
              '"field": {"byte": 0}, "offset": -40, "unit": "degC"%s}')
    (tmp_path / "manifest.json").write_text(
        '{"havne_plugin": 1, "id": "gauged", "name": "Gauged", "version": "1.0.0", '
        '"requires": {"core": "0.1.0"}, "source": {"name": "Havne\'s tests"}, '
        '"tiers": ["data"], "ecus": [{"id": "pcm", "rx": "7E8"}], '
        '"variants": [{"id": "plain", "files": ["a.json"]}, '
        '{"id": "gauged", "files": ["b.json"]}]}')
    (tmp_path / "a.json").write_text('{"havne_signals": 1, "signals": [%s]}' % (signal % ""))
    (tmp_path / "b.json").write_text(
        '{"havne_signals": 1, "signals": [%s]}'
        % (signal % ', "gauge": {"min": 40, "max": 150, "warn_high": 130}'))
    plugin = havne.Plugin(str(tmp_path))
    assert plugin.gauge("oil") is None, "the first variant's, when none is named"
    assert plugin.gauge("oil", "gauged") == havne.Gauge(40.0, 150.0, None, 130.0)
    assert plugin.gauge("no_such_signal", "gauged") is None
