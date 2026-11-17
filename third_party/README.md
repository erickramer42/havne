# Third-party source

Code from other projects that Havne compiles into its core. Each folder
is one project at one pinned version, its files byte for byte as that
project published them; what Havne adds (the build lists, a port file,
patches) sits beside the folder, never in it.

| Folder | Project | Version | License | Used for |
|---|---|---|---|---|
| `wamr/` | [WAMR](https://github.com/bytecodealliance/wasm-micro-runtime), the WebAssembly Micro Runtime | `WAMR-2.4.5` (commit `25bd7eb63e828e4bd242cc9b38d260b4b31c6605`) | Apache-2.0 WITH LLVM-exception (`wamr/LICENSE`) | The sandbox a conversation plugin's code runs in ([ADR-0066](../docs/adr/0066-conversation-tier-and-write-gates.md) section 2.1) |

## WAMR

- **What is here:** the classic interpreter, the loader, the runtime's
  common part, its allocator and utilities, and the ports for Windows,
  Linux, macOS (which iOS uses) and Android. `scripts/vendor_wamr.sh`
  names every folder kept and every file left out, and why.
- **What is compiled:** `wamr.cmake` lists each source and each switch.
  WAMR's own build scripts are not used and not kept.
- **`wamr_port.c`** is Havne's: the few functions WAMR's kept sources
  name in files that are left out.
- **Checking it:** `scripts/vendor_wamr.sh --check` clones the tag,
  confirms its commit, and compares the folder with it file by file.
  That needs the network and is run by hand. Every build checks the
  folder without it: `wamr.sha256` lists each file's SHA-256 (the script
  writes it with the folder), and ctest's
  `VendoredTest.WamrIsItsListedBytes` (`scripts/vendored_check.py`)
  fails on a file changed, added or gone. So an edit inside `wamr/` fails
  the suite; a change there is a patch beside it (below).
- **Moving the pin:** change the tag and commit in the script, run it,
  build every tree, and change the version in this table, in the
  notices and in the SBOM's component.
- **One sanitizer check is left out for WAMR's objects:**
  UndefinedBehaviorSanitizer's alignment check (`-fno-sanitize=alignment`
  in `wamr.cmake`, and again in `fuzz/CMakeLists.txt`). The classic
  interpreter keeps a frame's label stack, which holds pointers, at
  4-byte steps, so the check fails in it on a 64-bit build; WAMR's own
  UBSan build (`build-scripts/config_common.cmake`) leaves the check out
  too, and upstream's pull request 5062 aligns that stack. Havne's own
  code keeps the check, and every other check stays on WAMR's. The flag
  is on the object library, so it covers `wamr_port.c` too, whose stubs
  read and write no pointer. This
  holds while every target is x86, x64 or 64-bit ARM, which take an
  unaligned access; a 32-bit ARM target needs the fix first. **When
  moving the pin,** see whether the release carries the fix and, if it
  does, drop the flag from both files.
- **Under GCC's AddressSanitizer, WAMR's locals stay on the real
  stack** (`--param=asan-use-after-return=0` in `wamr.cmake`, for GCC
  alone). WAMR's stack check compares the address of a local with the
  thread's stack boundary; GCC's ASan keeps such a local in a fake stack
  on the heap, to catch its use after a return, and every call into a
  module then ends as "native stack overflow". Havne's own code keeps
  that detection. Clang's ASan leaves the local in place, so Clang
  builds, the fuzzers among them, take no flag for it.
- **A fix upstream has not released** is a numbered patch in
  `wamr-patches/` (`0001-<what>.patch`, with the upstream issue or pull
  request in its first lines), which the script applies in name order
  and `--check` expects. There is none while `wamr-patches/` is absent.

Tools that read Havne's own sources skip `third_party/`: clang-tidy,
CodeQL, the docs lint, and the compiler's warnings (it is built with
them off, so a warnings-as-errors build fails on Havne's code alone).
