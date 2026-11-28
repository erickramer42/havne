import hashlib
import json
import os
import sys
import time

import pytest

import havne
from conftest import PLUGINS, ROOT, cli, havne_cli

MERGE = os.path.join(ROOT, "tests", "fixtures", "merge")
ENGINE = os.path.join(MERGE, "engine-part.json")
TRANS = os.path.join(MERGE, "trans-part.json")
OBD2_RPM = "7E8:410C:2.0:16"
PART_RPM = "7E8:62F40C:3.0:16"
TFT = "7E9:621E1C:3.0:8"


def merged_name(base, parts):
    text = "".join(p + "\n" for p in ([base] if base else []) + parts)
    return "merged." + hashlib.sha256(text.encode()).hexdigest()[:16]


def test_a_merge_reads_its_parts_in_order():
    m = havne.SignalFile.merge([havne.SignalFile(path=ENGINE), havne.SignalFile(path=TRANS)],
                               base="obd2")
    assert m.profile == merged_name("obd2", ["user.engine-part", "user.trans-part"])
    assert [(p.profile, p.name, p.format, p.plugin, p.signals, p.modules)
            for p in m.parts()] == [("user.engine-part", "Engine part", 2, None, 2, 1),
                                    ("user.trans-part", "Trans part", 3, None, 1, 1)]
    assert m.parts()[0].source.name == "Made up for the merge tests"
    assert [(x.rx, x.tx, x.id, x.part) for x in m.modules()] == [
        (0x7E8, 0x7E0, "pcm", "user.engine-part"), (0x7E9, 0x7E1, "tcm", "user.trans-part")]
    assert m.notes() == [havne.Note(
        "beside_base", "'RPM' (Engine part) is read beside obd2's 'RPM': a merged set "
        "replaces no obd2 signal", PART_RPM, OBD2_RPM)]
    assert m.part_of(TFT).profile == "user.trans-part"
    assert m.part_of(OBD2_RPM) is None, "obd2's signal belongs to no part"
    assert m.def_module(TFT) == 1
    assert m.replaced_keys(TFT) == []
    with pytest.raises(LookupError):
        m.part_of("no_such_key")


def test_a_module_says_the_session_it_declares():
    text = json.dumps({"havne_signals": 1, "name": "S", "source": {"name": "t"},
                       "ecus": [{"id": "pcm", "rx": "7E8"},
                                {"id": "tcm", "rx": "7E9", "session": "extended"},
                                {"id": "abs", "rx": "7EA", "session": "default"}],
                       "signals": [{"key": "a", "name": "A", "ecu": "tcm",
                                    "read": {"service": "22", "number": "D100"},
                                    "field": {"byte": 0}}]})
    assert [x.session for x in havne.SignalFile(text).modules()] == [None, "extended", "default"]


def test_modules_reads_obd2s_modules_and_none_of_its_signals():
    trans = havne.SignalFile(path=TRANS)
    m = havne.SignalFile.merge([trans], base="modules")
    assert m.profile == merged_name("obd2 modules", ["user.trans-part"])
    assert [x.rx for x in m.modules()] == [0x7E8, 0x7E9]
    assert m.modules()[0].part is None
    assert m.keys() == [TFT]
    alone = havne.SignalFile.merge([trans])
    assert alone.profile == merged_name(None, ["user.trans-part"])
    assert [x.rx for x in alone.modules()] == [0x7E9]
    with pytest.raises(ValueError):
        havne.SignalFile.merge([trans], base="obd2-modules")


def test_a_plugins_variant_is_a_part():
    plugin = havne.Plugin(os.path.join(PLUGINS, "example-sim"))
    m = havne.SignalFile.merge([(plugin, "base"), havne.SignalFile(path=TRANS)])
    part = m.parts()[0]
    assert (part.profile, part.plugin, part.version, part.variant) == (
        "plugin.example-sim.base", "example-sim", plugin.version, "base")
    variant = plugin.variant_file("base")
    assert variant.profile == "plugin.example-sim.base" and variant.parts() == []


def test_a_refused_merge_says_why_in_cores_words():
    trans = havne.SignalFile(path=TRANS)
    with pytest.raises(havne.Error) as refused:
        havne.SignalFile.merge([trans, trans])
    assert "a set holds a part once" in str(refused.value)


def test_expr_names_are_the_names_as_written_in_read_order():
    assert havne.expr_names("boost * 2 + obd2.rpm - boost") == ["boost", "obd2.rpm"]
    with pytest.raises(havne.Error):
        havne.expr_names("boost *")


def test_needs_are_the_files_as_written():
    needing = havne.SignalFile(json.dumps({
        "havne_signals": 3, "name": "Needing", "source": {"name": "t"},
        "needs": ["tft"],
        "signals": [{"key": "hot", "name": "Hot", "kind": "computed", "expr": "tft - 90"}]}))
    assert needing.needs() == ["tft"]
    m = havne.SignalFile.merge([havne.SignalFile(path=TRANS), needing])
    assert "hot" in m.keys()


def test_the_merge_is_havne_clis_byte_for_byte(tmp_path):
    if not havne_cli():
        pytest.skip("HAVNE_CLI names no havne-cli")
    out = tmp_path / "merged.json"
    cli("plugins", "together", "--together", "obd2-modules,user.trans-part,"
        "plugin.example-sim.base", "--signals", TRANS, "--plugin",
        os.path.join(PLUGINS, "example-sim"), "--out", out)
    m = havne.SignalFile.merge([havne.SignalFile(path=TRANS),
                                (havne.Plugin(os.path.join(PLUGINS, "example-sim")), "base")],
                               base="modules")
    assert out.read_bytes() == m.text.encode("utf-8")


def test_a_mock_modules_state_is_open():
    with havne.Session("mock://x") as s:
        assert s.module_state(0) == havne.ModuleState("0x7E8", False, False)


ROOM = {
    "havne_signals": 1, "name": "Room", "source": {"name": "t"},
    "ecus": [{"id": "m1", "rx": "7E9"}, {"id": "m2", "rx": "7EA"},
             {"id": "m3", "rx": "7EB"}, {"id": "door", "rx": "7EC"}],
    "signals": [{"key": f"s{i}", "name": f"N s{i}", "ecu": e,
                 "read": {"service": "22", "number": f"100{i - 1}"},
                 "field": {"byte": 0}}
                for i, e in ((1, "m1"), (2, "m2"), (3, "m3"), (4, "door"))],
}


@pytest.mark.skipif(sys.platform != "win32", reason="the fake J2534 DLL is Windows'")
def test_a_module_the_adapter_has_no_room_for_says_so(monkeypatch):
    fake = os.environ.get("HAVNE_FAKE_J2534_DLL", "")
    assert fake and os.path.isfile(fake), "ctest names the fake DLL"
    monkeypatch.setenv("ECUIO_FAKE_MODULE_DIDS",
                       "7E9:1000=11;7EA:1001=12;7EB:1002=13;7EC:1003=14")
    monkeypatch.setenv("ECUIO_FAKE_FILTER_GATING", "1")
    monkeypatch.setenv("ECUIO_FAKE_FILTER_MAX", "3")
    room = havne.SignalFile(json.dumps(ROOM))
    uri = "j2534://" + fake.replace("\\", "/") + f"?profile={room.profile}&identify=0&diagnose=0"
    with havne.Session(uri) as s:
        s.start()
        until = time.monotonic() + 10
        while not s.module_state(3).no_room and time.monotonic() < until:
            time.sleep(0.05)
        assert s.module_state(3) == havne.ModuleState("0x7EC", False, True)
        assert not s.module_state(0).no_room
