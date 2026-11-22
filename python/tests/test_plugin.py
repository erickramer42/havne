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
# variants each give their own signal's gauge and format. A plugin names
# the core it requires. A signal file wraps to a packed plugin, a variant
# goes in and comes out (ADR-0064), each as havne-cli writes it, the
# lineage kept and an ECU's rename said; each variant names its source,
# and its text reads back as its set. A variant says the key each of its
# signals is kept under (ADR-0065 section 9). A plugin's Dashboard pages
# come in the manifest's order, the same from its folder and packed.

import json
import os
import shutil
import threading
import time

import pytest

import havne
import havne.signals
from havne._core import lib

from conftest import PLUGINS, ROOT, SIGNALS, cli, havne_cli, line_of


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
    manifest says 0.7.2, the example's is read from its own manifest"""
    packed = havne.Plugin(os.path.join(ROOT, "tests", "fixtures", "plugins", "packed.json"))
    assert packed.requires_core == (0, 7, 2)
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


def test_a_plugin_says_the_key_each_signal_of_a_variant_is_kept_under():
    p = havne.Plugin(os.path.join(PLUGINS, "example-sim"))
    base = p.keys()
    assert base[0] == "7E8:4104:2.0:8", "obd2's first signal: the variant extends it"
    assert base[-2:] == ["7E9:62F411:3.0:8", "boost"], (
        "a DID by its address, a computed signal by its author's key")
    assert len(set(base)) == len(base)
    newer = p.keys("my2019")
    assert "7E9:62F412:3.0:16" in newer and "7E9:62F412:3.0:16" not in base
    assert p.key_of("Oil Temp") == "7E8:62F40D:3.0:8"
    assert p.key_of("Clutch Slip", "my2019") == "7E9:62F412:3.0:16"
    assert p.key_of("7E9:62F412:3.0:16", "my2019") == "7E9:62F412:3.0:16"
    with pytest.raises(havne.Error, match="no signal with the key or name 'Clutch Slip'"):
        p.key_of("Clutch Slip", "base")


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


def test_a_plugin_says_a_variant_s_expression(tmp_path):
    (tmp_path / "manifest.json").write_text(
        '{"havne_plugin": 1, "id": "chained", "name": "Chained", "version": "1.0.0", '
        '"requires": {"core": "0.1.0"}, "source": {"name": "Havne\'s tests"}, '
        '"tiers": ["data"], "ecus": [{"id": "pcm", "rx": "7E8"}], '
        '"variants": [{"id": "plain", "files": ["a.json"]}, '
        '{"id": "chain", "files": ["a.json", "b.json"]}]}')
    (tmp_path / "a.json").write_text(
        '{"havne_signals": 1, "signals": [{"key": "oil", "name": "Oil", '
        '"read": {"service": "22", "number": "F40D"}, "field": {"byte": 0}, '
        '"offset": -40, "unit": "degC"}]}')
    (tmp_path / "b.json").write_text(
        '{"havne_signals": 2, "signals": ['
        '{"key": "hot", "name": "Hot", "kind": "computed", "expr": "if(oil > 100, 1, 0)"}, '
        '{"key": "hot_twice", "name": "Hot Twice", "kind": "computed", "expr": "hot * 2"}]}')
    plugin = havne.Plugin(str(tmp_path))
    assert plugin.expr("hot") is None, "the first variant has no such signal"
    assert plugin.expr("oil", "chain") is None, "a number signal has none"
    assert plugin.expr("hot", "chain") == "if(oil > 100, 1, 0)"
    assert plugin.expr("hot_twice", "chain") == "hot * 2"
    assert plugin.expr("no_such_signal", "chain") is None
    assert plugin.format() == 1, "the first variant's, when none is named"
    assert plugin.format("chain") == 2, "the newest among its files"


def fixture_text(name):
    """A signal-file fixture's text, its line ends LF (the lineage hashes
    the bytes, and a Windows checkout gives CRLF)"""
    with open(os.path.join(SIGNALS, name), "rb") as f:
        return f.read().replace(b"\r\n", b"\n").decode("utf-8")


def loaded(tmp_path, text, name="p.json"):
    path = tmp_path / name
    path.write_text(text, encoding="utf-8", newline="\n")
    return havne.Plugin(str(path))


def test_a_signal_file_wraps_and_a_variant_goes_in_and_out(tmp_path):
    wrapped = havne.Plugin.wrap(fixture_text("extras.json"))
    p = loaded(tmp_path, wrapped, "a.json")
    assert (p.id, p.version, p.variants) == ("desk-extras", "1.0.0", ["base"])
    assert len(p.lineage) == 32 and int(p.lineage, 16) >= 0

    put = havne.Plugin.put_variant(wrapped, fixture_text("gauges.json").encode("utf-8"))
    assert isinstance(put, havne.PutVariant) and put.notes == ""
    two = loaded(tmp_path, put.text, "b.json")
    assert two.variants == ["base", "gauged-extras"] and two.version == "1.1.0"

    renamed = havne.Plugin.put_variant(put.text, fixture_text("modules.json"), "modules")
    assert "'tcm' is 'tcm_2'" in renamed.notes, renamed.notes
    dropped = havne.Plugin.drop_variant(renamed.text, "gauged-extras")
    three = loaded(tmp_path, dropped, "c.json")
    assert three.variants == ["base", "modules"] and three.version == "1.3.0"
    assert two.lineage == three.lineage == p.lineage, "kept by put and drop"


def test_the_calls_give_the_bytes_havne_cli_writes(tmp_path):
    havne_cli()
    (tmp_path / "extras.json").write_text(fixture_text("extras.json"), encoding="utf-8",
                                          newline="\n")
    (tmp_path / "modules.json").write_text(fixture_text("modules.json"), encoding="utf-8",
                                           newline="\n")
    line_of(cli("plugins", "wrap", tmp_path / "extras.json", "--id", "mine",
                "--variant", "desk", "--out", tmp_path / "a.json"), "written")
    written = line_of(cli("plugins", "put", tmp_path / "a.json", tmp_path / "modules.json",
                          "--out", tmp_path / "b.json"), "written")
    wrapped = havne.Plugin.wrap(fixture_text("extras.json"), id="mine", variant="desk")
    put = havne.Plugin.put_variant(wrapped, fixture_text("modules.json"))
    assert (tmp_path / "a.json").read_bytes() == wrapped.encode("utf-8")
    assert (tmp_path / "b.json").read_bytes() == put.text.encode("utf-8")
    assert written["notes"] == put.notes and written["variant"] == "desk-modules"


def test_a_match_takes_its_codes_as_text_or_a_list(tmp_path):
    for wmi in ("ZZZ,ZZ9ABC", ["ZZZ", "ZZ9ABC"]):
        doc = json.loads(havne.Plugin.wrap(fixture_text("extras.json"), wmi=wmi,
                                           years=(2015, 2020)))
        assert doc["match"] == [{"wmi": ["ZZZ", "ZZ9ABC"], "years": [2015, 2020]}]
    doc = json.loads(havne.Plugin.wrap(fixture_text("extras.json"), wmi="ZZZ"))
    assert doc["match"] == [{"wmi": ["ZZZ"]}], "no years, every year"
    assert "match" not in json.loads(havne.Plugin.wrap(fixture_text("extras.json")))
    for lower in ("zzz", b"zzz", ["zzz", "zz9abc"]):
        doc = json.loads(havne.Plugin.wrap(fixture_text("extras.json"), wmi=lower))
        assert doc["match"][0]["wmi"] == (["ZZZ", "ZZ9ABC"] if isinstance(lower, list)
                                          else ["ZZZ"]), "upper case, as havne-cli"


def test_what_core_refuses_raises_its_words():
    with pytest.raises(havne.Error) as refused:
        havne.Plugin.wrap(fixture_text("refused.json"))
    assert refused.value.call == "ecuio_plugin_wrap" and "line " in refused.value.detail
    with pytest.raises(havne.Error) as no_wmi:
        havne.Plugin.wrap(fixture_text("extras.json"), years=(2015, 2020))
    assert "wmi" in no_wmi.value.detail
    wrapped = havne.Plugin.wrap(fixture_text("extras.json"))
    with pytest.raises(havne.Error) as last:
        havne.Plugin.drop_variant(wrapped, "base")
    assert last.value.call == "ecuio_plugin_drop_variant" and last.value.detail
    with pytest.raises(havne.Error) as bad:
        havne.Plugin.put_variant("{}", fixture_text("gauges.json"))
    assert bad.value.call == "ecuio_plugin_put_variant"


def test_each_variant_names_its_source(tmp_path):
    f = havne.SignalFile(path=os.path.join(SIGNALS, "modules.json"))
    assert f.source == havne.Source("Written by hand for Havne's tests", "", "",
                                    "CC-BY-SA-4.0", "", True)
    put = havne.Plugin.put_variant(havne.Plugin.wrap(fixture_text("gauges.json")),
                                   fixture_text("modules.json"), "modules")
    p = loaded(tmp_path, put.text)
    assert p.source() == havne.Source("Written by hand for Havne's tests", "", "", "", "",
                                      False)
    assert p.source("modules").license == "CC-BY-SA-4.0"
    assert p.source("modules").share_alike is True
    assert havne.Plugin(os.path.join(PLUGINS, "example-sim")).lineage is None, \
        "a manifest written by hand has none"


def test_a_variants_text_reads_back_and_moves_into_another_plugin(tmp_path):
    put = havne.Plugin.put_variant(havne.Plugin.wrap(fixture_text("extras.json")),
                                   fixture_text("gauges.json"))
    p = loaded(tmp_path, put.text)
    text = p.variant_text("gauged-extras")
    assert json.loads(text)["havne_signals"] in (1, 2)
    again = havne.SignalFile(text)
    assert again.name == "Desk extras" and again.gauge("coolant") is not None
    mine = loaded(tmp_path, havne.Plugin.put_variant(
        havne.Plugin.wrap(fixture_text("modules.json")), text, "copied").text, "m.json")
    assert mine.variants == ["base", "copied"]
    assert mine.gauge("coolant", "copied") == p.gauge("coolant", "gauged-extras")
    assert p.variant_text() == p.variant_text("base")


def test_a_plugins_pages_are_its_manifests_the_same_packed(tmp_path):
    folder = os.path.join(ROOT, "tests", "fixtures", "plugins", "pages")
    p = havne.Plugin(folder)
    pages = p.pages()
    assert [(path, form) for path, form, _ in pages] == [
        ("pages/engine-phone.json", "phone"), ("pages/engine-desktop.json", "desktop")]
    phone = json.loads(pages[0][2])
    assert phone["form"] == "phone" and phone["pages"][0]["name"] == "Engine"
    with open(os.path.join(folder, "manifest.json"), encoding="utf-8") as f:
        manifest = json.load(f)
    manifest["files"] = {}
    for rel in ["data/base.json", "data/turbo.json"] + manifest["pages"]:
        with open(os.path.join(folder, rel), encoding="utf-8") as f:
            manifest["files"][rel] = json.load(f)
    packed = tmp_path / "pages.havne-plugin.json"
    packed.write_text(json.dumps(manifest, ensure_ascii=False), encoding="utf-8")
    assert havne.Plugin(str(packed)).pages() == pages
    assert havne.Plugin(os.path.join(PLUGINS, "example-sim")).pages() == []
