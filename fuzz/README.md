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
| `fuzz_replay_file` | Arbitrary rows after a `frames.csv` header (ADR-0017), written to a scratch file in `TEMP`/`TMPDIR` and opened through `ecuio_session_create("replay://…")`, whose attach reads the whole file (ADR-0023). The status is `ECUIO_OK`, with a row count, or `ECUIO_ERR_LOAD_FAILED`, with a one-line detail that never holds the path; two creates agree. It runs at file I/O's pace, a few hundred inputs a second. |
| `fuzz_socketcan_wire` | SocketCAN's parsers of outside input (`core/src/socketcan_wire.h`, ADR-0032), the first byte picking one: `?filter=` text (at most 8 filters, each ID within its width and flagged 29-bit exactly when above 0x7FF, the mask within that width and carrying the width flag), the kernel's `RTM_NEWLINK` payload (the name and kind terminated; what discovery lists and the verdict on the link agree with it), or a kernel frame (the length within its kind's room, only the flags it can carry). Pure code, so it runs on every platform though the transport is Linux's. Each input is a heap copy of its exact size; two runs agree. |
| `fuzz_diagnostics` | The request engine reading an ECU's diagnostics and clearing them (`core/src/request_engine.h`, `diagnostics.h`; ADR-0037, ADR-0038), the input a script: each request answered by a timeout, raw bytes, a positive reply with an arbitrary tail (up to 128 bytes, past the 32 codes a list holds) or a negative one, a pending code among them, with Read again, rescans and clears (at the report's generation or one off it) between. `04` goes out only as the next request after the clear's `01 0C` was answered 0 rpm. After every step the report stays within its caps (codes, monitors, freeze-frame values, items), each list holds no more than it reported, every code and monitor it holds has a name, its values are finite, and it reads complete only with nothing left to ask; two runs agree. |

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
cmake --build build-fuzz-msvc --config RelWithDebInfo --target fuzz_decode_signal fuzz_validate_set fuzz_uds_query fuzz_elm327_reply fuzz_replay_file fuzz_socketcan_wire fuzz_diagnostics
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
cmake --build build-fuzz --target fuzz_decode_signal fuzz_validate_set fuzz_uds_query fuzz_elm327_reply fuzz_replay_file fuzz_socketcan_wire fuzz_diagnostics
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
