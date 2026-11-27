LEVEL_01 = bytes([0x5A, 0x17, 0xC3, 0x08])
BASE = "mock://sim?gateway=1&behind=7E9,7EA"


def own(req):
    return bytes([0x40 | len(req)]) + req


def module(req):
    return bytes([len(req)]) + req


def functional(req):
    return bytes([0x20 | len(req)]) + req


def clock(half_seconds, to=0):
    return bytes([0x80 | (half_seconds << 2) | to])


def gateway_run(uri, items):
    """fuzz_sim's gateway form: an even byte, G, the URI's length and the
    URI, then each item (fuzz_sim_gateway.h's run())"""
    text = uri.encode()
    assert len(text) < 256
    return b"\x00G" + bytes([len(text)]) + text + b"".join(items)


def key_01(key):
    return own(b"\x27\x02" + bytes(key))


def sim_gateway_seeds(sim_key):
    enter, seed = own(b"\x10\x03"), own(b"\x27\x01")
    made_up = own(sim_key(0x01))
    write, read = module(b"\x2E\xF1\x90\x01"), module(b"\x22\xF1\x90")
    gated = BASE + "&route=write:gated,reads:gated-silent"
    unlock = [enter, seed, made_up]
    wrong = key_01(b"\x00\x00\x00\x00")
    xor = bytes(b ^ 0xA5 for b in LEVEL_01)
    add = bytes((b + (0x01, 0x02)[i % 2]) & 0xFF for i, b in enumerate(LEVEL_01))
    pairs = "&sa=key:01020304:A1A2A3A4,05060708:B1B2B3B4"
    return {
        "gateway-gated-unlock": gateway_run(gated + "&sa=key",
                                            [write, read] + unlock + [write, read]),
        "gateway-pairs-in-turn": gateway_run(
            gated + pairs,
            [enter, seed, seed, key_01(b"\xB1\xB2\xB3\xB4"), write, seed, enter,
             seed, key_01(b"\xA1\xA2\xA3\xA4"), write]),
        "gateway-xor": gateway_run(gated + "&sa=xor:A5",
                                   [enter, seed, key_01(xor), write]),
        "gateway-add": gateway_run(gated + "&sa=add:0102",
                                   [enter, seed, key_01(add), write]),
        "gateway-never-delay": gateway_run(
            gated + "&sa=never:2000",
            [enter] + [seed, made_up] * 3 + [seed, clock(4), seed, own(b"\x3E\x00"),
                                             clock(1), seed, made_up]),
        "gateway-sa-delay": gateway_run(
            gated + "&sa=key&sa_delay=1500",
            [enter] + [seed, wrong] * 3 + [seed, clock(3), own(b"\x3E\x00"),
                                           seed, made_up, write]),
        "gateway-s3-at-a-route": gateway_run(gated + "&sa=key",
                                             unlock + [write, clock(11), write]),
        "gateway-keep-any": gateway_run(
            gated + "&sa=key&gwopt=keep-any",
            unlock + [clock(8), read, clock(8), read, clock(8), write]),
        "gateway-heard-only-on-its-id": gateway_run(
            gated + "&sa=key", unlock + [clock(8), read, clock(8), write]),
        "gateway-functional-aloud": gateway_run(
            BASE + "&route=reads:refuse.11,write:refuse,22:refuse.31@7EA"
                   "&gwopt=aloud,unsuppressed",
            [functional(b"\x22\xF1\x90"), functional(b"\x2E\xF1\x90\x01"),
             functional(b"\x19\x02\xFF")]),
        "gateway-functional-quiet": gateway_run(
            BASE + "&route=reads:refuse.11,write:refuse&gwopt=aloud",
            [functional(b"\x22\xF1\x90"), functional(b"\x2E\xF1\x90\x01")]),
        "gateway-relock": gateway_run(gated + "&sa=key&faults=relock:2",
                                      unlock + [write, write, write, seed, made_up,
                                                write]),
        "gateway-module-entries": gateway_run(
            BASE + "&route=reads:refuse.31@7E9,22:gated.7E@7EA,prog:silent,"
                   "10:gated-silent&sa=key",
            [read, clock(0, 1), read, module(b"\x34\x00\x44"), module(b"\x10\x03")]
            + unlock + [read, clock(0, 0), read]),
        "gateway-session-locks": gateway_run(
            gated + "&sa=key", unlock + [write, own(b"\x10\x01"), write] + unlock
            + [own(b"\x10\x03"), write]),
        "gateway-without-sa": gateway_run(
            "mock://sim?gateway=6F0:6F8&behind=7E9&route=write:refuse",
            [enter, seed, own(b"\x22\xF1\x90"), write]),
    }


def sim_gateway_uris():
    return {
        "uri-gateway": BASE + "&route=write:gated.7E@7E9,prog:gated-silent,"
                              "reads:refuse.31&sa=key:01020304:A1A2A3A4&sa_delay=900"
                              "&gwopt=aloud,unsuppressed,keep-any&faults=relock:3",
        "uri-gateway-masks": "mock://sim?gateway=6F0:6F8&sa=xor:0102030405060708090A0B0C0D0E0F10",
        "uri-gateway-refused": "mock://sim?gateway=1&route=write:gated",
        "uri-gateway-pending-code": BASE + "&route=write:refuse.78",
        "uri-gateway-silent-code": BASE + "&route=write:silent.31",
        "uri-gateway-behind-twice": "mock://sim?gateway=1&behind=7E9,7E9",
        "uri-gateway-not-behind": BASE + "&route=write:refuse@7EB",
        "uri-gateway-pair-of-5": BASE + "&sa=key:0102030405:0A0B0C0D0E",
        "uri-gateway-pairs-9": BASE + "&sa=key:" + ",".join(["01020304:0A0B0C0D"] * 9),
        "uri-gateway-long-delay": BASE + "&sa=key&sa_delay=600001",
        "uri-gateway-unsuppressed-alone": BASE + "&gwopt=unsuppressed",
        "uri-gateway-functional-id": "mock://sim?gateway=7DF:6F8",
        "uri-gateway-delay-alone": BASE + "&sa_delay=900",
    }
