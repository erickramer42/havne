# Fuzzing

Three [libFuzzer](https://llvm.org/docs/LibFuzzer.html) harnesses
exercise the parts of core that take arbitrary bytes, arbitrary signal
definitions and typed URI text, under AddressSanitizer (and
UndefinedBehaviorSanitizer with Clang):

| Harness | What it checks |
|---|---|
| `fuzz_decode_signal` | Arbitrary def geometry and payloads against `ecuio_decode_signal` and `ecuio_validate_signal`. Every return code is one of the six defined; two identical calls agree, NaN bits included; nothing is written unless the decode succeeds; and a def the validator accepts fails only on the payload's bounds or its mux key. The payload is a heap copy of exactly `payload_len` bytes, so ASan reports any read past it. |
| `fuzz_validate_set` | Arbitrary sets (defs, channels, NULL arrays, reserved fields, the layout version) against `ecuio_validate_signal_set`. The code is one this core defines; the `ecuio_validation_t` out matches the return, names the element its code says it names, and zeroes its reserved bytes; two calls agree. |
| `fuzz_uds_query` | Arbitrary text as a `?did=` query (more keys when it holds `&`) through the query split and the `uds-generic` builder (`core/src/uds_generic.h`, ADR-0020). The status is `ECUIO_OK` or `ECUIO_ERR_BAD_URI`, a refusal leaves a terminated detail line, and a success builds a set the validator accepts, within every limit, whose polled head derives one request per def and whose scanned DIDs all fit the engine's scan; two calls agree. |

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
cmake --build build-fuzz-msvc --config RelWithDebInfo --target fuzz_decode_signal fuzz_validate_set fuzz_uds_query
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
cmake --build build-fuzz --target fuzz_decode_signal fuzz_validate_set fuzz_uds_query
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
