# File/layer: fuzz/seeds_operation.py | Python 3, no packages; fuzz/seeds.py imports it
# Testing: FUZZ: CI's fuzz job, fuzz_operation started from what it writes
#
# fuzz_operation's seeds: a plan and a script, in the harness's own form.
# Two bytes name the block a 75 answers (the low one first); one byte
# holds the steps (its low part, one to three) and the identifiers (its
# high part, none to two); each step is a shape byte (bit 0 an upload,
# bit 1 the programming session, bit 2 three address bytes, bits 3 and 4
# the size bytes less one), its bytes less one in two bytes and, for a
# read, its chunk less one in two. The script follows: a control byte
# ahead of each step of the run (FF a cancel, FE a cancel while the
# request is out, 40 a full sink, 3F a session refused), and after it,
# for a request, an answer byte (its low three bits: 3 a silence, 4 a
# refusal with its code and its wait in 300 ms, 5 one bit wrong, 6 a byte
# short or long, 7 bytes of the script's own) with what it takes.


def step(upload=False, programming=False, bytes_=1000, chunk=256, size_bytes=2):
    shape = (1 if upload else 0) | (2 if programming else 0) | (size_bytes - 1) << 3
    out = bytes([shape, (bytes_ - 1) & 0xFF, (bytes_ - 1) >> 8])
    if not upload:
        out += bytes([(chunk - 1) & 0xFF, (chunk - 1) >> 8])
    return out


def plan(steps, idents=2, block=0x0402):
    return bytes([block & 0xFF, block >> 8, (len(steps) - 1) | idents << 4]) + b"".join(steps)


def right(n):
    """n requests answered as the module answers"""
    return bytes(2 * n)


def operation_seeds():
    read = plan([step()])
    upload = plan([step(upload=True, bytes_=4096, size_bytes=4)])
    small = plan([step(upload=True, bytes_=600, size_bytes=4)], idents=0, block=0x0A)
    return {
        "read_whole": read,
        "upload_whole": upload,
        "upload_counter_wraps": plan([step(upload=True, bytes_=2400, size_bytes=4)],
                                     idents=0, block=0x0A),
        "two_sessions": plan([step(), step(programming=True, bytes_=64, chunk=64),
                              step(upload=True, bytes_=2000, size_bytes=4)]),
        # The second chunk unanswered twice, then busy once
        "read_asked_again": read + right(3) + bytes([0, 3, 0, 3]),
        "read_busy": read + right(3) + bytes([0, 4, 0x21, 0, 0, 4, 0x21, 0]),
        "read_refused": read + right(4) + bytes([0, 4, 0x31, 0]),
        "read_unlock_lost": read + right(4) + bytes([0, 4, 0x33, 0]),
        "read_pending_then_silent": read + right(2) + bytes([0, 4, 0x78, 10]),
        "read_pending_past_its_bound": read + right(2) + bytes([0, 4, 0x78, 250]),
        "read_short": read + right(3) + bytes([0, 6]),
        "read_long": read + right(3) + bytes([0, 0x86]),
        # One bit wrong in the second pass's first chunk: the passes differ
        "read_differs": read + right(6) + bytes([0, 5]),
        "ident_changed": read + right(10) + bytes([0, 0x25]),
        "read_cancel": read + right(4) + bytes([0xFF]),
        "read_cancel_while_out": read + right(4) + bytes([0xFE, 0]),
        "sink_full": read + bytes([0x40, 0x40, 0, 0, 0x40, 0, 0]),
        "session_refused": plan([step()]) + bytes([0x3F]),
        "upload_block_lost": small + right(3) + bytes([0, 3]),
        "upload_wrong_counter": small + right(3) + bytes([0, 7, 3, 0x76, 0x09, 0x00]),
        "upload_refused": small + bytes([0, 4, 0x70, 0]),
        "upload_unanswered": small + bytes([0, 3]),
        "upload_block_of_nothing": plan([step(upload=True, bytes_=600, size_bytes=4)],
                                        idents=0, block=2),
        "upload_another_block": small + bytes([0, 7, 3, 0x75, 0x10, 0x08]),
        "upload_cancel": small + right(3) + bytes([0xFF]),
        "upload_cancel_while_out": small + right(3) + bytes([0xFE, 0]),
        "upload_exit_refused": plan([step(upload=True, bytes_=8, size_bytes=4)],
                                    idents=0, block=0x0A) + right(2) + bytes([0, 4, 0x24, 0]),
    }
