# File/layer: python/tests/test_plugin.py | pytest
# Testing: HARNESS, PythonTest.test_plugin: the example plugin through the
#          package (ADR-0052)
#
# plugins/example-sim loaded from its folder (core reads each file
# through the package's callback, on the loading thread), its variants
# listed, one registered, and a session decoding through it.

import os

import pytest

import havne

from conftest import PLUGINS


def test_the_example_plugin_loads_and_a_session_decodes_through_a_variant():
    p = havne.Plugin(os.path.join(PLUGINS, "example-sim"))
    assert p.id == "example-sim"
    assert p.variants[0] == "base" and len(p.variants) >= 2
    assert p.register("base") == "plugin.example-sim.base"
    with havne.Session("mock://py", plugin=p, variant=p.variants[1]) as s:
        assert "profile=plugin.example-sim." + p.variants[1] in s.uri
        assert s.signals, "the variant's signals"


def test_a_folder_with_no_plugin_is_refused(tmp_path):
    with pytest.raises(OSError):
        havne.Plugin(str(tmp_path))
    (tmp_path / "manifest.json").write_text('{"havne_plugin": 1}', encoding="utf-8")
    with pytest.raises(havne.Error) as e:
        havne.Plugin(str(tmp_path))
    assert e.value.name == "INVALID_ARGUMENT"
    assert "manifest.json" in e.value.detail
