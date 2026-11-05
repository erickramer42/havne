# Fuzzing

[libFuzzer](https://llvm.org/docs/LibFuzzer.html) harnesses exercise
the parts of core that take arbitrary bytes, arbitrary signal
definitions, typed URI text and data from outside the process (an
adapter's replies, a recording), under AddressSanitizer (and
UndefinedBehaviorSanitizer with Clang):

| Harness | What it checks |
|---|---|
| `fuzz_decode_signal` | Arbitrary def geometry and payloads against `ecuio_decode_signal` and `ecuio_validate_signal`. Every return code is one of the six defined; two identical calls agree, NaN bits included; nothing is written unless the decode succeeds; and a def the validator accepts fails only on the payload's bounds or its mux key. The payload is a heap copy of exactly `payload_len` bytes, so ASan reports any read past it. |
| `fuzz_validate_set` | Arbitrary sets (defs, channels, NULL arrays, reserved fields, the layout version) against `ecuio_validate_signal_set`. The code is one this core defines; the `ecuio_validation_t` out matches the return, names the element its code says it names, and zeroes its reserved bytes; two calls agree. |
| `fuzz_uds_query` | Arbitrary text as a `?did=` query (more keys when it holds `&`) through the query split and the `uds-generic` builder (`core/src/uds_generic.h`, ADR-0020). The status is `ECUIO_OK` or `ECUIO_ERR_BAD_URI`, a refusal leaves a terminated detail line, and a success builds a set the validator accepts, within every limit, whose polled head derives one request per def and whose scanned DIDs all fit the engine's scan; two calls agree. |
| `fuzz_elm327_reply` | Arbitrary bytes as an ELM327's output (serial, TCP or an app's Bluetooth stream) through the line reader, the line kinds, the frame parser and ISO-TP reassembly (`core/src/elm327_protocol.h`), the first byte picking the addressing mode and an `AT PPS` parameter, with the voltage and `AT PPS` parsers on every line. Each line stays within `ELM327_LINE_MAX` and terminated; a frame line holds 1 to 8 bytes and an ID of its mode; a reassembled message fits a frame, is copied at its exact size (so ASan sees a read past it) and is never longer than its First Frame said; a prompt leaves no message under way; two runs agree. |
| `fuzz_replay_file` | Arbitrary rows after a `frames.csv` header (ADR-0017), written to a scratch file in `TEMP`/`TMPDIR` and opened through `ecuio_session_create("replay://…")`, whose attach reads the whole file (ADR-0023). The status is `ECUIO_OK`, with a row count, or `ECUIO_ERR_LOAD_FAILED`, with a one-line detail that never holds the path; two creates agree. An input starting with `F` carries a `?faults=` query on its first line (ADR-0041): its refusal is `ECUIO_ERR_BAD_URI`, or `ECUIO_ERR_UNKNOWN_PROFILE` for a profile the text adds after an `&`, with a one-line detail, and a fault set that parses draws within its bounds for frames of every length (a bit within the data, a truncation shorter, nothing more for a dropped frame). It runs at file I/O's pace, a few hundred inputs a second. |
| `fuzz_socketcan_wire` | SocketCAN's parsers of outside input (`core/src/socketcan_wire.h`, ADR-0032), the first byte picking one: `?filter=` text (at most 8 filters, each ID within its width and flagged 29-bit exactly when above 0x7FF, the mask within that width and carrying the width flag), the kernel's `RTM_NEWLINK` payload (the name and kind terminated; what discovery lists and the verdict on the link agree with it), or a kernel frame (the length within its kind's room, only the flags it can carry). Pure code, so it runs on every platform though the transport is Linux's. Each input is a heap copy of its exact size; two runs agree. |
| `fuzz_diagnostics` | The request engine reading an ECU's diagnostics and clearing them (`core/src/request_engine.h`, `diagnostics.h`; ADR-0037, ADR-0038), the input a script: each request answered by a timeout, raw bytes, a positive reply with an arbitrary tail (up to 128 bytes, past the 32 codes a list holds) or a negative one, a pending code among them, with Read again, rescans and clears (at the report's generation or one off it) between. `04` goes out only as the next request after its gate: the clear's `01 0C` answered 0 rpm, or, with the caller's word on the engine (any of the call's flags is fuzzed), its `01 0D` answered 0 km/h, or asked at all with the word on a moving vehicle too. After every step the report stays within its caps (codes, monitors, freeze-frame values, items), each list holds no more than it reported, every code and monitor it holds has a name, its values are finite, and it reads complete only with nothing left to ask; two runs agree. |
| `fuzz_isotp` | ISO-TP (`core/src/isotp.h`, ADR-0040), the first byte picking one: arbitrary frames at arbitrary times to the timed receiver across eight IDs (every slot within its bounds, a message never past 4095 bytes or its slot, a Single Frame's within its frame, nothing left past N_Cr after an expiry, nothing left after the end of a reply), or a round trip from the sender to the receiver under the input's Flow Control frames (the sender keeps STmin, the block size and the sequence numbers; a send that ends arrives whole and the same; one overflowed or abandoned sends nothing more). Each message is read through an exact-size copy; two runs agree. |
| `fuzz_vin` | VIN decode (`core/src/vin.cpp`, ADR-0043): arbitrary text as a VIN under an arbitrary latest model year, and as a manufacturer code. A refusal leaves the info zeroed; a decode is 17 characters, its year 0 or 1980-2039 and never past the bound once the note's later half, its check digit 0-9 or X matching position 9 exactly when flagged, its code 3 or 6 long, its reserved room zero; a make is never NULL; two calls agree. |
| `fuzz_dtc` | A typed trouble code (`ecuio_dtc_parse`, `core/src/dtc_titles.cpp`, ADR-0044): arbitrary text. A refusal leaves the code untouched; a parse formats back to the same text through `ecuio_dtc_format`, and its title is never NULL. |
| `fuzz_sim` | The simulated ECU (`core/src/sim_ecu.h`, ADR-0046): arbitrary text as its URI, and a run of arbitrary requests off the bus in either mode under faults, its car stepped between them. A refused URI says why; a parsed one holds a 17-character VIN and faults in range; a reply stays within its buffer and is a positive reply to the request's service or a three-byte negative one naming it; two ECUs given the same requests answer the same. |
| `fuzz_recording` | A recording's files ([ADR-0049](../docs/adr/0049-recordings-in-core.md) section 5), the first byte picking one: each line through every parser of `core/src/recording_rows.h` (`signals.csv`'s and `markers.csv`'s rows, `events.log`'s signal, UTC and activity lines, its summary and drop lines), where a parsed name stays within 31 bytes, a unit within 15 and decimal places within 3, and two parses agree, NaN bits included; or a folder, the input cut at 0x1E bytes into `events.log`, `signals.csv`, `markers.csv` and `frames.csv` in a scratch folder in `TEMP`/`TMPDIR`, opened through `ecuio_recording_open` and checked through `ecuio_recording_check` (section 6). Each status is `ECUIO_OK` or `ECUIO_ERR_LOAD_FAILED` with a one-line detail that never holds the path; an open recording's counts agree with its queries (one column over everything holds every sample, from the summaries or the raw rows), its names and units are terminated, and `text_at` and `find` succeed within the signal's times; each finding is of a known kind and within the summary's duration, a whole-bus one carries no ID and an ISO-TP one is on ISO 15765-4's IDs, the summary counts what was passed, and a check stopped at its first finding counts that one; two opens and two checks agree. |
| `fuzz_request_bus` | A requesting session's state across many requests ([ADR-0040](../docs/adr/0040-socketcan-requests-isotp.md)): the request engine (`core/src/request_engine.h`) and core's ISO-TP driven as the SocketCAN worker drives them, on a simulated bus and clock, against one ECU per channel answering through `tests/fake_ecu.h` and core's ISO-TP sender. The first byte picks the profile (`obd2` on one or two channels, packing on or off; `uds-generic` with DIDs polled and scanned, 11-bit or 29-bit); the rest, request by request, each reply's fate (intact, silent, response pending first, a negative code, garbage, on the other channel's ID, a wrong sequence number, a stall past N_Cr, cut short, a frame twice, a Single Frame longer than its frame), stray frames, the clock's steps, and Read again, rescans, clears and re-probes between. After every step each request is one the engine may send (`04` only while clearing) and a Single Frame; no worker would spin (nothing out, nothing due, `next_event_us` 0); a reply on one channel changes no other channel's request; every report stays within its caps, its generation never going back; ISO-TP's slots stay within bounds and nothing outlives N_Cr. Then intact replies alone must end every channel's walk, identification, diagnostics and DID scan within simulated minutes, and after a Read again leave every identification item answered or refused and every report complete. It runs at a few hundred inputs a second; two runs agree. |
| `fuzz_json` | The strict JSON reader that signal files go through (`core/src/json.h`, [ADR-0051](../docs/adr/0051-signal-files.md) section 1): arbitrary bytes, the first byte picking the depth bound (1 to 64) and, with its top bit, a size bound at half the input. A refusal has one line of words and a position within the input's lines (none only for the size bound) and leaves the value null; a document stays within the depth bound, holds only valid UTF-8 (checked apart from the reader's own check), finite numbers, an exact `int64_t` only for an integer, no key twice in an object, and positions that move forward in document order; written back out and read again it is the same document; two parses agree. |
| `fuzz_signal_file` | The signal-file loader (`ecuio_signal_file_load`, [ADR-0051](../docs/adr/0051-signal-files.md)): arbitrary bytes, best seeded with real files (`tests/fixtures/signal-files/`). Two loads agree, status and words; a refusal leaves no file and one line of words; a loaded file's set passes the validator, its profile name registers, and every def decodes a payload carrying its own key through the decoder its kind takes. |

They aren't ctest tests: a time-bounded fuzzer in the default pass
would make a green suite stop meaning green. The CI `fuzz` job runs
each for 60 s on Linux with Clang and uploads any crashing input.

## Running them

`-DECUIO_BUILD_FUZZERS=ON` instruments `ecuio-core` and the harnesses
itself, so no compiler flags need passing. It needs a tree of its own
(`HAVNE_BUILD_CORE_ONLY=ON`, `BUILD_TESTING=OFF`), because nothing but
a harness can link the instrumented core.

**Windows, with the Visual Studio build tools** (MSVC's experimental
libFuzzer, Visual Studio 2022 17.0 or later; nothing else to install):

```sh
cmake -B build-fuzz-msvc -A x64 -DHAVNE_BUILD_CORE_ONLY=ON -DBUILD_TESTING=OFF -DECUIO_BUILD_FUZZERS=ON
cmake --build build-fuzz-msvc --config RelWithDebInfo --target fuzz_decode_signal fuzz_validate_set fuzz_uds_query fuzz_elm327_reply fuzz_replay_file fuzz_socketcan_wire fuzz_diagnostics fuzz_isotp fuzz_vin fuzz_dtc fuzz_sim fuzz_recording fuzz_request_bus fuzz_json fuzz_signal_file
build-fuzz-msvc/fuzz/RelWithDebInfo/fuzz_decode_signal.exe -max_total_time=60
```

The build copies the ASan runtime DLL next to each harness, so they run
from any shell. MSVC has no UndefinedBehaviorSanitizer; the CI job
covers that half.

**Linux, with Clang** (the CI job's recipe; the fuzzer and sanitizer
runtimes are in `libclang-rt-<major>-dev` on Ubuntu):

```sh
cmake -B build-fuzz -DCMAKE_BUILD_TYPE=RelWithDebInfo -DHAVNE_BUILD_CORE_ONLY=ON \
  -DBUILD_TESTING=OFF -DECUIO_BUILD_FUZZERS=ON -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build-fuzz --target fuzz_decode_signal fuzz_validate_set fuzz_uds_query fuzz_elm327_reply fuzz_replay_file fuzz_socketcan_wire fuzz_diagnostics fuzz_isotp fuzz_vin fuzz_dtc fuzz_sim fuzz_recording fuzz_request_bus fuzz_json fuzz_signal_file
./build-fuzz/fuzz/fuzz_decode_signal -max_total_time=60
```

AppleClang ships no libFuzzer, and clang-cl links through `link.exe`,
so configure refuses both.

**A crash** leaves `crash-<hash>` in the working directory (the CI job
uploads its `artifacts/` folder). Pass the file to the harness to replay
it under the debugger: `fuzz_decode_signal crash-<hash>`.

## Scorecard

OpenSSF Scorecard's Fuzzing check counts a C++ source that defines
`LLVMFuzzerTestOneInput`, in a repository where C++ is a prominent
language, so these harnesses on the default branch are what it looks
for. The CI job is what makes them find bugs.
