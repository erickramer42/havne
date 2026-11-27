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


def kernel_plan(steps, idents=2, block=0x0402, stays=True, quiet=True, blob=600,
                wire=4095):
    head = plan(steps, idents, block)
    shape = (0 if stays else 1) | (2 if quiet else 0)
    return (head[:2] + bytes([head[2] | 0x80]) + head[3:] +
            bytes([shape, (blob - 1) & 0xFF, (blob - 1) >> 8, wire & 0xFF, wire >> 8]))


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
        **kernel_seeds(),
    }


def kernel_seeds():
    """A kernel read: the functional 10 82, the session entered, two
    identifiers, the helper's 34, 36 and 37 and its 31, the ranges, the
    session left and the identifiers again"""
    read = kernel_plan([step()])
    head = right(1) + bytes(1) + right(2)
    started = head + right(4)
    return {
        "kernel_read_whole": read,
        "kernel_upload_whole": kernel_plan([step(upload=True, bytes_=2000, size_bytes=4)]),
        "kernel_no_quieter": kernel_plan([step()], quiet=False),
        "kernel_blocks_many": kernel_plan([step()], blob=3000, wire=258),
        "kernel_counter_wraps": kernel_plan([step()], idents=0, blob=8192, wire=34),
        "kernel_helper_again": kernel_plan([step(upload=True, bytes_=600, size_bytes=4),
                                            step(upload=True, bytes_=900, size_bytes=4)],
                                           stays=False, block=0x0A),
        "kernel_wire_too_small": kernel_plan([step()], wire=2),
        "kernel_block_named_small": kernel_plan([step()], block=0x02),
        "kernel_quieter_silent": read + bytes([0, 3]),
        "kernel_quieter_refused": read + bytes([0, 4, 0x12, 0]),
        "kernel_download_refused": read + head + bytes([0, 4, 0x70, 0]),
        "kernel_download_another_block": read + head + bytes([0, 7, 4, 0x74, 0x20, 0x00, 0x40]),
        "kernel_block_lost": read + head + right(1) + bytes([0, 3]),
        "kernel_block_wrong_counter": read + head + right(1) + bytes([0, 7, 2, 0x76, 0x09]),
        "kernel_exit_refused": read + head + right(2) + bytes([0, 4, 0x24, 0]),
        "kernel_start_refused": read + head + right(3) + bytes([0, 4, 0x22, 0]),
        "kernel_start_other_routine": read + head + right(3) + bytes([0, 5]),
        "kernel_cancel_in_download": kernel_plan([step()], blob=3000, wire=258) + head +
        right(2) + bytes([0xFE, 0]),
        "kernel_cancel_at_start": read + head + right(3) + bytes([0xFF]),
        "kernel_session_refused": read + right(1) + bytes([0x3F]),
        "kernel_ident_unread_after": read + started + right(8) + bytes(1) + bytes([0, 4, 0x31, 0]),
        "kernel_ident_changed_after": read + started + right(8) + bytes(1) + bytes([0, 0x25]),
    }


def sim_helper(size=20, wrong=False):
    """The simulated ECU's made-up helper: HAVNEKRN, then the big-endian
    sum of the bytes after its 12-byte head"""
    body = bytes((i * 31 + 7) & 0xFF for i in range(12, size))
    total = sum(body) + (1 if wrong else 0)
    return b"HAVNEKRN" + total.to_bytes(4, "big") + body


def sim_kernel_seeds(sim_run, sim_key):
    """fuzz_sim's kernel= runs: the config byte, K and the faults byte
    (once, storm, silent, refuse from bit 0), then the requests"""
    def k(faults, requests):
        run = sim_run(0x02, requests)
        return run[:1] + b"K" + bytes([faults]) + run[1:]

    entry = [b"\x10\x02", b"\x27\x01", sim_key(0x01)]
    image = sim_helper()
    download = [b"\x34\x00\x44\x00\xF0\x00\x00" + len(image).to_bytes(4, "big"),
                b"\x36\x01" + image, b"\x37", b"\x31\x01\x02\x01"]
    code = b"\x23\x24\x00\x08\x00\x00\x00\x10"
    upload = [b"\x35\x00\x44\x00\x08\x00\x00\x00\x00\x00\x20", b"\x36\x01", b"\x37"]
    bad = list(download)
    bad[1] = b"\x36\x01" + sim_helper(wrong=True)
    return {
        "kernel-read-code": k(0, entry + download + [code]),
        "kernel-withheld-then-read": k(0, entry + [code] + download + [code]),
        "kernel-upload-code": k(0, entry + download + upload + [code]),
        "kernel-once": k(1, entry + download + upload + [code]),
        "kernel-storm": k(2, entry + download + [code]),
        "kernel-silent": k(4, entry + download + [code, b"\x3E\x00"]),
        "kernel-refuse": k(8, entry + download + [code]),
        "kernel-bad-sum": k(0, entry + bad + [code]),
        "kernel-session-left": k(0, entry + download + [b"\x10\x01"] + entry + [code]),
        "kernel-download-twice": k(0, entry + download + download[:1]),
        "kernel-out-of-ram": k(0, entry + [b"\x34\x00\x44\x00\xF0\x40\x00\x00\x00\x00\x14"]),
        "kernel-locked": k(0, [b"\x10\x02"] + download + [code]),
        "kernel-quieter": k(0, entry) + b"\x12\x10\x82" + sim_run(0x02, download + [code])[1:],
    }
