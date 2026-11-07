# File/layer: python/examples/did_scan.py | Python 3.11+, an example of the havne package
# Testing: HARNESS, PythonTest.test_did_scan (the mock; the sim's module on vcan0)
#
# Which identifiers an ECU answers: core's own DID scan (ADR-0020) over
# a range, through any session, Service 0x22 only, so it reads and never
# writes. Each DID is asked once; an answer joins the session's rotation
# and is printed here once, as text when its bytes are printable, as hex
# otherwise; a refusal is counted.
#
#   python did_scan.py socketcan://vcan0 --range F180-F1FF
#   python did_scan.py mock://scan --ecu 7E8 --ecu 7E9
#
# Each --ecu names a receive ID (the ECU's reply ID, 7E8 by default); a
# module other than 7E8 is asked through uds-generic's <ecu>: prefix.
# Exit 0 once every ECU's scan has completed, 1 when --seconds run out
# first, 2 for a bad argument.

import argparse
import sys
import time

import havne

SCAN_COMPLETE = havne.c.ECUIO_SUPPORT_SCAN_COMPLETE


def scan_uri(uri, first, last, ecus):
    """The URI under uds-generic with one did= range per ECU"""
    if "profile=" in uri:
        raise ValueError("the scan sets the profile itself (uds-generic): leave it out of the URI")
    keys = ["profile=uds-generic"]
    for rx in ecus:
        prefix = "" if rx == 0x7E8 else f"{rx:X}:"
        keys.append(f"did={prefix}{first:04X}-{last:04X}")
    return uri + ("&" if "?" in uri else "?") + "&".join(keys)


def shown(text):
    """A hex value's bytes, as text when every byte is printable"""
    data = bytes.fromhex(text)
    body = data.rstrip(b"\0")
    if body and all(0x20 <= b < 0x7F for b in body):
        return len(data), f'"{body.decode("ascii")}"'
    return len(data), text


def did_scan(uri, first=0xF180, last=0xF1FF, ecus=(0x7E8,), seconds=60.0, out=sys.stdout):
    """Scan [first, last] on each ECU; {(rx, did): bytes text} of the
    answers, and whether every scan completed"""
    found = {}
    with havne.Session(scan_uri(uri, first, last, ecus)) as s:
        values = s.values()
        s.start()
        until = time.monotonic() + seconds
        done, settle = False, None
        # A stream that ends (a replay's last frame, the session closed)
        # brings no more answers: the scan stops with it, as read_pids.py
        while time.monotonic() < until and not values.ended:
            for v in values.read(0.1):
                name = v.name.split()
                if v.kind != havne.c.ECUIO_VALUE_HEX or "DID" not in name:
                    continue
                rx = int(name[0], 16) if name[0].startswith("0x") else 0x7E8
                key = (rx, int(name[-1], 16))
                if key not in found:
                    found[key] = v.text
                    n, what = shown(v.text)
                    print(f"{rx:X}  {key[1]:04X}  {n} byte{'' if n == 1 else 's'}  {what}",
                          file=out, flush=True)
            if not done:
                done = complete(s, ecus, found, last - first + 1)
                if done:
                    settle = time.monotonic() + 0.5   # the last answers' values
            elif time.monotonic() >= settle:
                break
        if values.ended and not done:
            print(f"the stream ended before the scan completed ({values.ended})",
                  file=out, flush=True)
        for sup in channels(s, ecus):
            if sup.scan_total:
                print(f"{sup.rx_id:X}  scanned {sup.scan_done} of {sup.scan_total}, "
                      f"found {sup.scan_found}, refused {sup.scan_refused}", file=out, flush=True)
            else:
                mine = sum(1 for rx, _ in found if rx == sup.rx_id)
                print(f"{sup.rx_id:X}  found {mine}: the transport answers DIDs itself, "
                      f"with no scan of core's to count", file=out, flush=True)
    return found, done


def complete(session, ecus, found, size):
    """Every ECU's scan done: core's count where its request engine scans
    (scan_total), else (the mock, which answers DIDs itself) an answer
    for every DID of the range"""
    sups = channels(session, ecus)
    if len(sups) < len(ecus):
        return False
    for sup in sups:
        if sup.scan_total:
            if not sup.flags & SCAN_COMPLETE:
                return False
        elif sum(1 for rx, _ in found if rx == sup.rx_id) < size:
            return False
    return True


def channels(session, ecus):
    """The support of each channel whose ECU is one of ecus"""
    count = session.info.channel_count
    return [sup for sup in (session.support(i) for i in range(count)) if sup.rx_id in ecus]


def main(argv=None):
    p = argparse.ArgumentParser(description="core's DID scan over a range, read-only")
    p.add_argument("uri")
    p.add_argument("--range", default="F180-F1FF", help="first-last, hex (default F180-F1FF)")
    p.add_argument("--ecu", action="append", help="an ECU's receive ID, hex (default 7E8)")
    p.add_argument("--seconds", type=float, default=60.0)
    args = p.parse_args(argv)
    try:
        first, last = (int(x, 16) for x in args.range.split("-"))
        ecus = tuple(int(x, 16) for x in (args.ecu or ["7E8"]))
        if not 0 <= first <= last <= 0xFFFF:
            raise ValueError("the range is first-last, within 0000-FFFF")
        scan_uri(args.uri, first, last, ecus)
    except ValueError as e:
        print(f"did_scan: {e}", file=sys.stderr)
        return 2
    _, done = did_scan(args.uri, first, last, ecus, args.seconds)
    if not done:
        print(f"did_scan: the scan didn't complete within {args.seconds:g} s", file=sys.stderr)
    return 0 if done else 1


if __name__ == "__main__":
    sys.exit(main())
