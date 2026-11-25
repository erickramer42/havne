# File/layer: python/tests/test_consent.py | pytest
# Testing: HARNESS, PythonTest.test_consent: a person's consent to a plugin
#          and a module's unlock through the package (ADR-0066's appendix
#          of 2026-11-23)
#
# The consent store's round trip in a folder of the case's own (give is
# refused unless it is shown the plugin's terms, a fingerprint given twice
# keeps its first second, withdraw one and all, a file that doesn't read
# is an error naming no path), core's lines, a plugin's fingerprint,
# publisher and terms, and the default path's rule. Sessions run on the
# ELM327 emulator (havne-cli sim --elm327 tcp:0): with no record applied
# no plugin code talks; with one it does, and an unlock (the fixture
# plugin's module computes the emulator's key, HAVNE_UNLOCK_FIXTURE writes
# it) opens the gated level once; the wrong key is a failed attempt and a
# second word inside the delay is TooSoon, nothing sent.

import hashlib
import json
import os
import subprocess
import time

import pytest

import havne

from conftest import ROOT, havne_cli

TALKER3 = os.path.join(ROOT, "tests", "fixtures", "plugins", "talker3.json")
FORMAT2 = os.path.join(ROOT, "tests", "fixtures", "plugins", "conversation.json")


def test_a_plugin_says_its_fingerprint_its_publisher_and_its_terms():
    p = havne.Plugin(TALKER3)
    assert len(p.fingerprint) == 64 and p.fingerprint == p.fingerprint.lower()
    assert p.publisher.name and p.publisher.terms_sha256
    assert p.terms
    assert hashlib.sha256(p.terms.encode("utf-8")).hexdigest() == p.publisher.terms_sha256
    assert p.conversation_ecus().keys() == {"pcm", "tcm"}


def test_a_plugin_with_no_publisher_has_no_terms():
    p = havne.Plugin(FORMAT2)
    assert p.publisher is None and p.terms is None
    assert len(p.fingerprint) == 64
    assert p.conversation_ecus().keys() == {"pcm", "tcm"}, "the module it would talk to"


def test_give_is_refused_unless_it_is_shown_the_plugins_terms(tmp_path):
    p = havne.Plugin(TALKER3)
    with havne.ConsentStore(tmp_path / "consent.json") as store:
        for shown in ("", p.terms + " ", "something else", p.terms[:-1]):
            with pytest.raises(ValueError, match="terms"):
                store.give(p, shown)
        assert store.records() == []
        assert not (tmp_path / "consent.json").exists()


def test_the_store_gives_finds_lists_and_withdraws(tmp_path):
    p = havne.Plugin(TALKER3)
    path = tmp_path / "consent.json"
    with havne.ConsentStore(path) as store:
        assert store.find(p.fingerprint) is None and store.latest("talker3") is None
        record = store.give(p, p.terms)
        assert record.id == "talker3" and record.version == "1.0.0"
        assert record.fingerprint == p.fingerprint
        assert record.terms_sha256 == p.publisher.terms_sha256
        assert record.publisher == p.publisher.name
        assert abs(record.given_utc - time.time()) < 60
        assert store.find(p.fingerprint.upper()) == record
        assert store.latest("talker3") == record
        assert store.records() == [record]
        time.sleep(1.1)
        assert store.give(p, p.terms) == record, "a fingerprint given again keeps its second"
        assert len(store.records()) == 1
    with havne.ConsentStore(path) as again:
        assert again.records() == [record], "the give was on the disk"
        again.withdraw("talker3", p.fingerprint)
        assert again.records() == []
        with pytest.raises(havne.Error) as no_record:
            again.withdraw("talker3")
        assert no_record.value.name == "INVALID_ARGUMENT"
    with havne.ConsentStore(path) as last:
        assert last.records() == []


def test_a_withdraw_of_the_id_takes_every_record_of_it(tmp_path):
    p = havne.Plugin(TALKER3)
    with havne.ConsentStore(tmp_path / "consent.json") as store:
        store.give(p, p.terms)
        store.withdraw("talker3")
        assert store.find(p.fingerprint) is None


def test_a_plugin_with_no_publisher_takes_no_consent(tmp_path):
    p = havne.Plugin(FORMAT2)
    with havne.ConsentStore(tmp_path / "consent.json") as store:
        with pytest.raises(havne.Error):
            store.give(p, "")
        assert store.records() == []


def test_a_fingerprint_is_32_bytes_of_hex(tmp_path):
    with havne.ConsentStore(tmp_path / "consent.json") as store:
        for bad in ("", "zz" * 32, "ab" * 31):
            with pytest.raises(ValueError, match="64 hex"):
                store.find(bad)


def test_a_store_that_doesnt_read_is_an_error_naming_no_path(tmp_path):
    path = tmp_path / "bad-consent.json"
    path.write_text("not a store", encoding="utf-8")
    with pytest.raises(havne.Error) as refused:
        havne.ConsentStore(path)
    assert refused.value.name == "LOAD_FAILED"
    assert "bad-consent" not in str(refused.value) and str(tmp_path) not in str(refused.value)
    assert path.read_text(encoding="utf-8") == "not a store", "the file is left as it was"


def test_a_closed_store_is_refused(tmp_path):
    store = havne.ConsentStore(tmp_path / "consent.json")
    store.close()
    store.close()
    with pytest.raises(ValueError, match="closed"):
        store.records()


def test_the_default_store_is_the_apps_in_the_settings_folder(tmp_path, monkeypatch):
    monkeypatch.setenv("HAVNE_SETTINGS_DIR", str(tmp_path / "home"))
    assert havne.consent_path() == os.path.join(str(tmp_path / "home"), "consent.json")
    p = havne.Plugin(TALKER3)
    with havne.ConsentStore() as store:
        store.give(p, p.terms)
    assert (tmp_path / "home" / "consent.json").is_file()


def test_the_lines_are_cores_words_by_class():
    conversation = havne.consent_lines("conversation")
    assert conversation and all(x.text for x in conversation)
    assert conversation[0].id == havne.c.ECUIO_CONSENT_LINE_HAVNE_1
    both = havne.consent_lines(["conversation", "read"])
    assert len(both) > len(conversation)
    assert [x.id for x in both if x.id < 10] != [], "the shared lines are there"
    with pytest.raises(ValueError):
        havne.consent_lines("flash")
    with pytest.raises(ValueError):
        havne.consent_lines([])


# The ELM327 emulator and a session over it ------------------------------

class Emulator:
    def __init__(self):
        self.p = subprocess.Popen([havne_cli(), "sim", "mock://sim?values=canned", "--elm327",
                                   "tcp:0", "--seconds", "120"],
                                  stdout=subprocess.PIPE, text=True)
        self.port = None
        for line in self.p.stdout:
            j = json.loads(line)
            assert j["type"] != "error", j
            if j["type"] == "elm327":
                self.port = int(j["listen"].rsplit(":", 1)[1])
                break
        assert self.port, "the emulator said where it listens"

    def uri(self, plugin_id="talker3"):
        return (f"elm327://127.0.0.1:{self.port}?profile=plugin.{plugin_id}.base"
                "&identify=0&diagnose=0&probe=0")

    def close(self):
        self.p.terminate()
        self.p.communicate(timeout=60)


@pytest.fixture
def emulator():
    havne_cli()
    emu = Emulator()
    yield emu
    emu.close()


def unlock_is(s, what):
    """The pcm's conversation once its unlock reads what, else None"""
    c = s.conversation("pcm")
    return c if c.unlock == what else None


def until(what, test, seconds=20):
    end = time.time() + seconds
    while time.time() < end:
        got = test()
        if got:
            return got
        time.sleep(0.05)
    raise AssertionError(f"timed out: {what}")


def test_a_session_applies_a_consent_only_with_a_record(tmp_path, emulator):
    p = havne.Plugin(TALKER3)
    with havne.ConsentStore(tmp_path / "consent.json") as store:
        with havne.Session(emulator.uri(), plugin=p) as s:
            with pytest.raises(havne.Error) as none:
                s.apply_consent(store)
            assert none.value.name == "CONSENT_REQUIRED"
            s.start()
            time.sleep(1.0)
            assert s.conversation("pcm").state == "none", "no record: no code talks"
        store.give(p, p.terms)
        with havne.Session(emulator.uri(), plugin=p) as s:
            s.apply_consent(store)
            s.start()
            until("the module's conversation talks",
                  lambda: s.conversation("pcm").state == "talking")
    with havne.Session("mock://py") as plain:
        with havne.ConsentStore(tmp_path / "other.json") as store, \
                pytest.raises(ValueError, match="plugin="):
            plain.apply_consent(store)


def test_an_ecu_is_named_by_the_plugins_id_or_a_channel_number(tmp_path, emulator):
    p = havne.Plugin(TALKER3)
    with havne.Session(emulator.uri(), plugin=p) as s:
        assert s.conversation("pcm").rx_id == p.conversation_ecus()["pcm"]
        channel = [rx for rx, _ in s.modules].index(p.conversation_ecus()["pcm"])
        assert s.conversation("pcm") == s.conversation(channel)
        with pytest.raises(ValueError, match="talks to no ECU"):
            s.conversation("nope")
        with pytest.raises(havne.Error) as bad:
            s.conversation(99)
        assert bad.value.name == "INVALID_ARGUMENT"


# An unlock, on the plugins tests/cli_unlock_fixture writes ----------------

def unlock_plugins(tmp_path):
    writer = os.environ.get("HAVNE_UNLOCK_FIXTURE")
    if not writer or not os.path.exists(writer):
        pytest.skip("no cli_unlock_fixture (HAVNE_UNLOCK_FIXTURE)")
    right, wrong = tmp_path / "right.json", tmp_path / "wrong.json"
    done = subprocess.run([writer, str(right), str(wrong)], capture_output=True)
    assert done.returncode == 0, done.stderr
    return havne.Plugin(str(right)), havne.Plugin(str(wrong))


def word(session, ecu, level, seconds=20):
    """The word given as soon as core can take it: the module's identity is
    read first (TooSoon with no wait) and the report moves meanwhile"""
    end = time.time() + seconds
    while time.time() < end:
        try:
            return session.unlock(ecu, level)
        except havne.TooSoon as soon:
            if soon.wait_ms:
                raise
        except havne.Error as e:
            if e.name != "REPORT_CHANGED":
                raise
        time.sleep(0.05)
    raise AssertionError("the word was never taken")


def session_with_consent(emu, plugin, tmp_path, name):
    store = havne.ConsentStore(tmp_path / f"{name}-consent.json")
    store.give(plugin, plugin.terms)
    s = havne.Session(emu.uri("talker"), plugin=plugin)
    s.security_store(str(tmp_path / f"{name}-security.json"), "bench")
    s.apply_consent(store)
    store.close()
    return s


def test_the_right_key_opens_the_level_and_the_word_goes_once(tmp_path, emulator):
    right, _ = unlock_plugins(tmp_path)
    with session_with_consent(emulator, right, tmp_path, "right") as s:
        s.start()
        until("talking", lambda: s.conversation("pcm").state == "talking")
        shown = s.conversation("pcm")
        assert shown.kept and shown.unlock in ("none", "waiting") and shown.failed == 0
        assert word(s, "pcm", 1) >= 0
        done = until("unlocked", lambda: unlock_is(s, "unlocked"))
        assert done.failed == 0 and done.level == 1
        s.unlock("pcm", 1)                    # open already: the report says so, no seed asked
        again = until("already unlocked", lambda: unlock_is(s, "already_unlocked"))
        assert again.failed == 0


def test_the_wrong_key_is_a_failed_attempt_and_the_delay_refuses_the_next(tmp_path, emulator):
    _, wrong = unlock_plugins(tmp_path)
    with session_with_consent(emulator, wrong, tmp_path, "wrong") as s:
        s.start()
        until("talking", lambda: s.conversation("pcm").state == "talking")
        word(s, "pcm", 1)
        failed = until("failed", lambda: unlock_is(s, "failed"))
        assert failed.failed == 1
        with pytest.raises(havne.TooSoon) as soon:
            s.unlock("pcm", 1)
        assert 0 < soon.value.wait_ms <= 60000


def test_a_stale_generation_changes_nothing(tmp_path, emulator):
    right, _ = unlock_plugins(tmp_path)
    with session_with_consent(emulator, right, tmp_path, "stale") as s:
        s.start()
        until("talking", lambda: s.conversation("pcm").state == "talking")
        with pytest.raises(havne.Error) as changed:
            s.unlock("pcm", 1, generation=s.conversation("pcm").generation + 1000)
        assert changed.value.name == "REPORT_CHANGED"
        assert s.conversation("pcm").unlock in ("none", "waiting")
