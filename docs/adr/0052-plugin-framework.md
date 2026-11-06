# ADR-0052: The plugin framework: one package, three tiers, data first

- **Status:** Accepted. The data tier's core built in v0.5.0 (the appendix).
- **Date:** 2026-11-05.
- **Builds on:** [ADR-0001](0001-qt-free-core-c-abi.md) (the Qt-free
  core, the C ABI as the boundary);
  [ADR-0013](0013-protocol-profiles.md) (profiles chosen by the URI,
  the URI as provenance, section 3's "what you connect to is a
  decision; what it supports is a discovery");
  [ADR-0015](0015-request-engine.md) (the request engine, its service
  allow-list); [ADR-0021](0021-vehicle-identification.md)
  (identification); [ADR-0027](0027-vehicle-profiles.md) (vehicles,
  the VIN kept on the device);
  [ADR-0038](0038-clearing-trouble-codes.md) (the clear's gates);
  [ADR-0041](0041-replay-fault-injection.md) (a recording as an ECU);
  [ADR-0043](0043-vin-decode.md) (VIN decode: the manufacturer code and
  model year); [ADR-0046](0046-simulated-ecu.md) (the simulated ECU);
  [ADR-0048](0048-semantic-versioning.md) (what a minor version may
  break); [ADR-0049](0049-recordings-in-core.md) (recordings);
  [ADR-0051](0051-signal-files.md) (signal files: the data tier's
  format).
- **Amends:** ADR-0013 (section 6 here replaces its section 5's callback
  table as the plugin seam). A note there.
- **Leads to:** v0.5.0's example plugin and `havne-cli plugins`; the
  signal-file format's version 2, a maker's trouble-code titles first;
  the conversation tier's build, with v0.8.0's discovery capture; the
  writes tier's own ADR (2.0).

## Context

ADR-0051 lets a user describe their own signals as data: a JSON file
that core reads from bytes, validates and registers as a profile
(`user.<slug>`). That covers one person's file for one car. A maker's
data is larger and shared: many DIDs on several modules, names,
scaling and enumerations, the maker's trouble-code titles, and
differences by model year and calibration. Beyond data, a maker's
modules need conversations Havne doesn't hold (an extended session,
tester present, security access, a gateway's routing), and eventually
writes (a DID written, a routine run).

ADR-0013 section 5 sketched a callback table for profiles and called
it the seed of the v0.5.0 plugin API. It was never built: a profile is
a signal set (`ecuio_profile_register(name, set, out)`), and core does
the rest. The question is whether one framework can carry data,
conversation logic and writes, which carry very different risk.

What any answer must keep:

- **Core never links Qt, and the C ABI is the boundary** (ADR-0001):
  a plugin is something core loads, never something the app loads
  around core.
- **Every platform:** iOS loads no native code at run time, and
  Android discourages it. Data loads everywhere.
- **The URI is complete provenance** (ADR-0013 section 2), and
  selection is never inferred from what the car says (section 3).
- **The request engine sends only reads** (ADR-0015 section 3,
  `ecuio_engine_service_allowed`, checked at compile time), and a
  clear only behind ADR-0038's gates.
- **Nothing on the network:** Havne sends nothing anywhere. A plugin
  is a file the user brings.
- **havne-cli is a first-class front end:** whatever a plugin offers
  reaches the app and the CLI alike, rendered the same generic way.

## Decision

### 1. One package format, three tiers of capability

A plugin is one package with one manifest, one registry entry, one
lifecycle and one test harness. What it may do is set by the tiers its
manifest declares, each with its own form and with core enforcing the
dangerous parts:

| Tier | Covers | Form | Enforced by core | Built |
|---|---|---|---|---|
| **1. Data** | A maker's DIDs and PIDs, names, scaling, enumerations, computed signals, which module answers what; a maker's trouble-code titles from the signal-file format's version 2 | ADR-0051 signal files, nothing else | The signal-file loader's refusals and the set validator; no code runs | v0.5.0 |
| **2. Conversation** | Extended session (10 03), tester present (3E), security access (27), gateway routing, dynamically defined DIDs (2C), periodic reads (2A), a maker's quirks | Code in a sandbox: a state machine that asks for addressed requests and takes replies; core does ISO-TP, timing and the transport | Every request checked against the manifest's services and core's ceiling (section 6); compute metered; no threads, sockets, files or clocks | With v0.8.0's discovery capture |
| **3. Writes** | Write DID (2E), routines (31), output control (2F), later programming (34, 36, 37) | A declarative plan core runs: preconditions, the backup, the steps, the read-back, the undo; code only computes a value or a key | Gates like the clear's, the backup before anything is sent, a confirmation naming what changes, the read-back, a journal | 2.0, under an ADR of its own |

The rule that holds the tiers apart: **tier 2 can never send a write.**
Writes go only through tier 3, which needs its own manifest tier, a
signature and the user's opt-in. The more a tier can do, the more of
it core runs itself.

### 2. The package

```
example-sim/
  manifest.json       what the plugin is, which cars, which tiers
  data/
    base.json         ADR-0051 signal files
    my2019.json
  fixtures/           what `plugins check` and CI run (section 8)
    values.json
```

- **The manifest is JSON,** read by ADR-0051's strict parser with the
  same rules (unknown keys refused, `notes` and `x-` keys free, line
  and column in every refusal, never the path). One grammar in core.
- **A packed form for phones and sharing:** one file,
  `<id>.havne-plugin.json`, the manifest with each data file inlined
  under `files` by its relative name. The app's Import button takes it
  the way it takes a signal file (bytes from Qt's file reading, so
  Android's `content://` and iOS's sandboxed URLs work). Fixtures
  aren't packed: a phone loads data, it doesn't run checks.
- **No archive format:** core reads no zip (a deflate reader is a
  large parsing surface to fuzz), and Qt's zip classes are private. A
  folder on the desktop and in the CLI, the packed file everywhere.

```json
{
  "havne_plugin": 1,
  "id": "example-sim",
  "name": "Example: the simulated car",
  "version": "1.0.0",
  "requires": { "core": "0.5.0" },
  "source": { "name": "Written for Havne's simulated ECU", "license": "MIT" },
  "tiers": ["data"],
  "match": [ { "wmi": ["ZZZ"], "years": [2015, 2030] } ],
  "ecus": [
    { "id": "pcm", "rx": "7E8" },
    { "id": "tcm", "rx": "7E9" }
  ],
  "variants": [
    { "id": "base", "files": ["data/base.json"] },
    { "id": "my2019", "years": [2019, 2030], "files": ["data/base.json", "data/my2019.json"] }
  ],
  "fixtures": ["fixtures/values.json"]
}
```

- **`havne_plugin`**, first and required, is the manifest format's
  version; a reader refuses one it doesn't know.
- **`id`**: 1-31 bytes of lower-case letters, digits and `-`. It names
  the profiles (section 4).
- **`version`**: the plugin's own `X.Y.Z`. **`requires.core`**: the
  oldest core that reads it, compared with `ecuio_version_string()`.
- **`source`**: ADR-0051's block, required, and copied into every
  data file's set the plugin registers, so attribution travels with
  each recording.
- **`tiers`**: `["data"]` in v0.5.0. A manifest naming a tier this core
  doesn't run is refused, never half-loaded.
- **`match`**: the manufacturer codes (`ecuio_vin_info_t.wmi`, three
  characters, or six when the third is 9) and an inclusive model-year
  range. It only proposes (section 5). The example plugin matches the
  simulated ECU's made-up VIN (`ZZZ`, model year 2021).
- **`ecus`**: ADR-0051's shape and rules (one ID width, the tx rule,
  at most `ECUIO_MAX_CHANNELS`). A data file in the package names
  these ECUs by `id` and declares none of its own, so a module's
  addresses are written once.
- **`variants`**: each a named, ordered list of the package's data
  files, merged at load by signal `key` (a later file's signal
  replaces an earlier one's with the same key and adds the rest), with
  an optional year range. Explicit variants keep the URI complete: the
  profile names the variant, so nothing is merged differently at the
  car than on the desk. OBDb's `default.json` plus model-year
  overrides imports as one variant per year range, so no merge depends
  on the car's year at connect.
- **Bounds**: 1 MiB a manifest, 16 MiB a package, 64 data files, 32
  variants. A path is relative, forward slashes, no `..`, no leading
  `/`, at most 127 bytes.

### 3. The data tier is ADR-0051's format

Every data file is an ADR-0051 signal file, loaded by the same loader,
refused by the same rules, decoded by the same worker code. This ADR
adds only what a package needs around them: the ECUs declared once in
the manifest, the variants that merge files, and the selection.
`extends` works as in ADR-0051 (a variant's first file may extend
`obd2`).

A maker's trouble-code titles are tier-1 data too. ADR-0051's format 1
carries signals only, and its **version 2 has the titles as its first
change**: a reader's coverage of a maker's modules needs their codes'
titles as much as their signals. Until format 2, a plugin carries
signals.

### 4. The registry: a variant is a profile

- **Loading registers nothing.** `ecuio_plugin_load` parses and
  validates the whole package (the manifest, every data file, every
  variant's merge and its request count) and holds it. A package with
  one bad file is refused whole, the refusal naming the file and its
  line and column.
- **Registering a variant makes a profile:** `plugin.<id>.<variant>`
  (`plugin.example-sim.my2019`), alongside ADR-0051's `user.<slug>`
  and the built-ins. The URI says `?profile=plugin.example-sim.my2019`,
  so a bench sheet's URI says exactly what decoded it.
- **One version per id.** Loading an id already loaded replaces it
  once no session uses its profiles (the registry keeps the sets'
  pointers, ADR-0051 section 4), and is refused while one does.
- **Where plugins are found:** the app keeps an imported plugin in its
  data folder, copied there as signal files are, and loads it from
  there at start; `havne-cli` takes explicit paths. There is no search
  path and no environment variable, so nothing loads that the user
  didn't name.
- **Provenance in recordings:** the merged set is copied into the
  recording as ADR-0051's `signals.json` with its SHA-256, and
  `events.log`'s header gains the plugin's id, version and variant.

### 5. Selection: the VIN proposes, the user decides

ADR-0013 section 3 holds: what a session decodes with is chosen, never
inferred from the car. The VIN narrows the choice:

- `ecuio_plugin_match(plugin, &vin_info, ...)` lists the variants
  whose `match` and `years` cover the decoded VIN (ADR-0043's
  manufacturer code and model year).
- **The app** shows the matching plugins and variants in the
  vehicle's section once the VIN is known (ADR-0027: the VIN stays on
  the device), and the user picks one for that vehicle, kept in the
  vehicle book. Connecting uses the picked profile's URI.
- **`havne-cli`**: `havne-cli plugins match <pkg> --vin <VIN>` prints
  the matching variants; `stream` takes `--plugin <pkg>` and the
  profile in the URI, never a guess.
- **Calibration-specific data** (by the part or software number,
  F187 and F188, or the CALID) needs an identification read before
  the choice, which is discovery. Manifest format 1 matches by maker
  code and model year only; matching by calibration waits for v0.8.0's
  discovery and a manifest format version that adds it.

### 6. The conversation tier (designed here, built with discovery)

- **Code runs in a WebAssembly interpreter,** the same on every
  platform, its memory its own and nothing reachable but what core
  hands it. Core meters its instructions per call, so a plugin can't
  stall the worker. The runtime is picked when the tier is built, by
  its size on a phone, its license and its maintenance; WAMR's
  interpreter and wasm3 are the candidates.
- **No native plugins in a release build.** A developer build may load
  a native module through the same table to debug a plugin; a release
  refuses one.
- **The table,** called on the worker only, every struct beginning
  with its size and the ABI version, append-only:

  ```c
  typedef struct ecuio_plugin_conversation_ops {
      uint32_t struct_size;
      uint32_t abi_version;
      int  (*begin)(void* ctx, const ecuio_plugin_ecu_t* ecu);       /* a module joins: session, security */
      int  (*next_request)(void* ctx, ecuio_plugin_request_t* out);  /* an addressed PDU, or none this cycle */
      void (*accept)(void* ctx, const ecuio_plugin_reply_t* reply);  /* a whole reply, or a timeout */
      void (*end)(void* ctx, const ecuio_plugin_ecu_t* ecu);         /* the session closes */
  } ecuio_plugin_conversation_ops;
  ```

  This table replaces ADR-0013 section 5's sketch as the plugin seam.
- **Every request is checked twice:** against the manifest's
  `services` (a plugin names what it sends, and `plugins check` lists
  it), and against core's ceiling for the tier, fixed at compile time
  with a `static_assert` as the request engine's list is. The ceiling
  allows 10 03 (extended session; never 10 02, programming), 3E, 27,
  22, 2C, 2A and 19. It never allows 2E, 2F, 31, 34 to 37, 3D, 14, 04,
  11, 28 or 85, whatever a manifest says.
- **Signals come from the data tier.** A conversation plugin opens the
  doors (session, security, routing); what it reads still decodes
  through its data files' defs. Its replies feed the same decode.
- **Its fixtures are recorded conversations,** replayed by the
  simulated ECU as ADR-0041's recording-as-ECU does, so a plugin's
  logic runs in CI with no car.
- **Signing** is decided with the tier.

### 7. The writes tier (bounded here, designed in its own ADR)

What this ADR fixes, so the earlier tiers don't paint it into a
corner:

- A write is an **operation**: a named plan with parameters,
  preconditions (engine off, vehicle stopped, battery voltage), the
  reads that back up what changes, the steps, the read-back that
  verifies, and how to undo. Core runs the plan; plugin code may only
  compute a value or a key.
- Core's gates run before any step: ADR-0038's for the clear, plus
  voltage; a backup is saved before the first byte is sent; the user
  confirms text that names every DID or routine and its new value; a
  journal records each step.
- Its own manifest tier (`"writes"`), the user's opt-in per operation,
  and a **signature**: a writes plugin is signed, and the key's owner
  and how the app shows it are settled in the tier's own ADR. A writes
  plugin is proven against the simulated ECU before any car sees it.
- No UI from plugins (section 9): an operation renders the same way in
  the app and `havne-cli`.

Tier 1 is not signed: it is data, validated whole at load, and its
`source` block says where it came from.

### 8. Fixtures and `plugins check`

`havne-cli plugins check <pkg>` (and CI for every plugin in the
repository) loads the package as a session would, then runs its
fixtures:

- **Data tier, value fixtures:** a list of `{ "variant", "signal",
  "value" }`. The simulated ECU answers each signal's request from the
  variant's own defs, its scaling inverted (ADR-0046's appendix of
  2026-11-05), a session decodes it on vcan or in-process, and the
  value must come back within half the field's step. So the scaling,
  the field and the module are checked together, with no car.
- **Conversation tier:** recorded conversations (section 6).
- **Required for a plugin in the repository; a warning for a user's
  own,** so a large import isn't refused for lacking hand-written
  examples.

### 9. No UI from plugins

Plugins describe signals, enumerations and, later, operations (name,
parameters, preconditions, confirmation text). The app and `havne-cli`
render them the same generic way; a plugin ships no QML and no
strings outside its data.

### 10. What stays out of plugins

- **Transports and buses** (K-line, J1850, DoIP): core work, since
  they touch timing and teardown. A plugin speaks requests and replies.
- **Fetching:** no plugin index, store or download. A plugin is a file
  the user brings; fetching would be Havne's first network request.
- **Real makes' plugins in this repository:** the repository's example
  is for the simulated car; makes' plugins stay with their authors.
- **Seed and key algorithms:** often a maker's secret; a user's own
  tier-2 plugin, never in the repository.

### 11. The C ABI for the data tier (v0.5.0)

```c
typedef struct ecuio_plugin ecuio_plugin_t;   /* opaque */

/* The host's reader for a folder package: the bytes of the file at a
   manifest-relative path, valid until the next call or load's return.
   NULL for a packed package. */
typedef int (*ecuio_plugin_read_t)(void* user, const char* path,
                                   const char** bytes, size_t* length);

ecuio_status_t ecuio_plugin_load(const char* manifest, size_t length,
                                 ecuio_plugin_read_t read, void* user,
                                 ecuio_plugin_t** out);

typedef struct ecuio_plugin_info {
    uint32_t struct_size;
    uint32_t tiers;              /* ECUIO_PLUGIN_TIER_DATA, ... */
    char id[32];
    char version[16];
    char name[64];
    uint32_t variant_count;
    uint32_t reserved[3];
} ecuio_plugin_info_t;

ecuio_status_t ecuio_plugin_info(const ecuio_plugin_t* plugin, ecuio_plugin_info_t* out);
ecuio_status_t ecuio_plugin_variant(const ecuio_plugin_t* plugin, uint32_t index,
                                    char* id, size_t cap);
ecuio_status_t ecuio_plugin_match(const ecuio_plugin_t* plugin,
                                  const ecuio_vin_info_t* vin,
                                  uint32_t* indices, uint32_t cap, uint32_t* count);
/* Registers variant index as profile "plugin.<id>.<variant>", its name
   written to name. */
ecuio_status_t ecuio_plugin_register(ecuio_plugin_t* plugin, uint32_t index,
                                     char* name, size_t cap);
void ecuio_plugin_free(ecuio_plugin_t* plugin);
```

- Core reads bytes and never a path; the host's callback reads the
  folder (the app with Qt, the CLI with stdio), so the same code runs
  on every platform.
- Refusals come through `ecuio_last_error_detail()`: the file, the line
  and column, the key.
- All additive: new functions, an opaque type and a sized struct. No
  layout or set change beyond ADR-0051's.
- `ecuio_plugin_free` unregisters the plugin's profiles and is refused
  (a new status, `ECUIO_ERR_IN_USE`) while a session uses one.

## Done when (v0.5.0's part)

- `ecuio_plugin_load` and the rest, with `fuzz_plugin_manifest` over the
  manifest and the packed form.
- `havne-cli plugins list <pkg>...`, `plugins check <pkg>`, `plugins
  match <pkg> --vin <VIN>`, and `stream --plugin <pkg>`.
- The example plugin for the simulated car in the repository, its
  value fixtures passing in CI against the simulated ECU on vcan,
  both modules.
- The app imports a packed plugin, shows the matches for a vehicle and
  connects with the picked variant.

## Alternatives considered

- **TOML for the manifest:** friendlier to write by hand, but a second
  grammar in core beside ADR-0051's JSON.
- **A zip package:** one file everywhere, but a deflate reader in core
  to write and fuzz, or a library to vet.
- **Selection by the VIN, automatic:** fewer taps, but it breaks
  ADR-0013 section 3, and a recording's URI would stop saying what
  decoded it.
- **Model-year overrides merged at run time, OBDb's way:** no variant
  list to write, but the merge would depend on the car's year at
  connect, outside the URI.
- **Matching by calibration in format 1:** closer to what a maker's
  data varies by, but it needs an identification read before the
  choice, which is v0.8.0's discovery.
- **A maker's trouble-code titles in v0.5.0:** they would grow format 1
  before its first release; format 2 takes them first instead.
- **Native plugins (shared libraries):** fast and simple on the
  desktop, impossible on iOS, and a buggy one takes the process down.
  Kept for developer builds only.
- **Lua or JavaScript for tier 2:** small interpreters, but a second
  language's sandbox to trust; WebAssembly takes code from any
  language into one.
- **A search path for plugins:** convenient, but it loads what the user
  didn't name.
- **One tier, full power, trusted plugins:** simpler, but data and a
  write would carry the same trust.

## Consequences

- A maker's data loads on every platform, iOS included, with no code
  and the same refusals as a user's own file.
- The URI names the plugin and the variant, so a recording says what
  decoded it, and selection stays a decision.
- The conversation tier's boundary (the sandbox, the service ceiling)
  is fixed before its code exists, so its build is a runtime choice,
  not a redesign.
- Writes stay out of reach of every tier but the third, which waits
  for its own ADR.
- A plugin can be checked with no car: its values through the
  simulated ECU, its conversations through recordings.
- The signal-file format's next version is spoken for: a maker's
  trouble-code titles come first in format 2.
- The repository never holds a make's plugin; an example stands in.

## Appendix: the data tier's core as built (2026-11-05)

`core/src/plugin.cpp` and `ecuio/plugin.h` build section 11, and settle
what it left open:

- **One build for both loaders.** A variant's signals, merged from its
  files by key, go through the signal-file loader's own rules
  (`core/src/signal_file_build.h`), so a plugin's signal is refused for
  exactly what a signal file's would be.
- **The free returns a status.** Section 11 declared
  `ecuio_plugin_free` `void` and refused it in use; it returns
  `ecuio_status_t`, `ECUIO_ERR_IN_USE` (20, new) with nothing changed
  while a session or a simulated ECU decodes with one of its sets. The
  registry counts each set's uses from the lookup that hands it out to
  the session's or the simulator's destroy, under its own lock, so no
  connect can come between the check and the unregister. A profile
  name registered to another set since is left alone.
- **Accessors for the CLI and the checks:** `ecuio_plugin_fixture` and
  the info's `fixture_count` (a folder's fixtures; `plugins check`
  reads them), and `ecuio_plugin_variant_set` and `_variant_find` (a
  variant's merged set, its def by key).
- **Variant ids** are 1 to 24 of `a`-`z`, `0`-`9` and `-`, a letter or
  digit at each end, so `plugin.<id>.<variant>` fits a profile name's
  63; a plugin's id is the same alphabet, 1 to 31.
- **A data file** declares no `ecus` (it names the manifest's by id);
  its `name` and `source` are optional, the manifest's `source` being
  the plugin's. Only a variant's first file `extends` a set. A key given
  twice in one file is refused; across a variant's files the later one
  replaces the earlier in its place.
- **The packed form** carries no `fixtures`, and every file under
  `files` must be in a variant; a folder's manifest takes no `files`.
- **Bounds:** 16 fixtures and 32 `match` entries of up to 64
  manufacturer codes each, beside section 2's; a data file is at most a
  signal file's 1 MiB.
- **Model years** in `match` and a variant's `years` run 1980 to 2039,
  `ecuio_vin_decode`'s range; a range never covers model year 0.
- **A refusal's words:** for a folder, the file's name in the package,
  then `line L, column C: ` and why; for a packed package, whose lines
  are one file's, the line and column alone. A file the host can't give
  is refused at the path in the manifest that names it.
- **`requires.core`** is compared with the core's own X.Y.Z, which on
  `dev` is the last release's until the release commit; the example
  plugin names the version its tree builds as, and the release commit
  raises it with the project's.

## Appendix: havne-cli's plugins as built (2026-11-05)

`havne-cli plugins list`, `check` and `match`, and `stream --plugin`
and `sim --plugin`, build section 5's CLI and section 8's check:

- **Core reads the fixture files.** The CLI reaches core through its
  public headers alone, so a fixture file is read by core's own JSON
  reader, `ecuio_plugin_values(plugin, bytes, length, values, cap,
  &count)`, one more additive function in `plugin.h`: format 1 is
  `{"havne_fixtures": 1, "values": [{"variant", "signal", "value"}]}`,
  1 to 4096 entries, each naming a variant of the plugin and a number
  or enum signal of that variant's by key (not text, hex or computed:
  the simulated ECU answers a field), the pair once in the file. Each
  entry comes back with its variant's index, its def, its key and its
  line; a refusal is the loader's `line L, column C: why`.
- **A fixture's run:** per variant, the simulated ECU on the
  interface `--on` names, `mock://check?profile=plugin.<id>.<variant>`,
  answers the variant's DIDs with the fixtures' values
  (`ecuio_sim_set_value`), and a `socketcan://` session through the
  same profile, with identification and the diagnostics off, decodes
  them back. A value passes within half its field's step (|scale|; an
  enum's step is 1), and fails when it decodes past that, when the
  simulated ECU can't answer it (not a Service 22 DID's field, or a
  value the field can't hold), or when none comes within 20 s.
- **Off Linux,** or without `--on`, the fixtures are listed as not
  run; with `--on` off Linux the simulated ECU's refusal is the
  command's error. A plugin with no fixture files is a warning on the
  check's summary line, never a failure, as section 8 has it for a
  user's own; `CliPluginRunTest` holds every plugin in the repository
  to at least one fixture, all passing, on CI's vcan.
- **`--plugin` needs the URI's `profile=`** to name one of the
  plugin's variants, every one of which it registers; anything else is
  a usage error naming them. `--plugin` and `--signals` together are
  too, since each gives the profile.
- **The exits:** a package refused is exit 3, as a refused signal file
  is; a fixture failed or a fixture file refused is exit 1; `plugins
  list` goes on past a refused package and exits 3 at the end.
## Appendix: a variant's recording (2026-11-05)

Section 4's provenance, as built:

- **A variant is one signal file too.** `ecuio_plugin_variant_file`
  gives it as an `ecuio_signal_file_t` (valid until the plugin is freed,
  never freed itself), so `ecuio_signal_file_name`, `_profile`
  (`plugin.<id>.<variant>`), `_find` and `_extends` read it and
  `ecuio_recorder_signal_file` copies it into a recording, the
  attach's check that its set is the session's unchanged.
- **Its text** is written at load: the plugin's `name`, a `notes` line
  naming the plugin, its version and the variant, the manifest's
  `source` and `ecus`, the first file's `extends` and the merged
  signals in their order, through `ecuio_json::write` (each value as it
  was read: a number keeps its kind and sign, and its value wherever 15
  significant digits hold it). The text is read back through the
  signal-file loader and kept only when the set it builds equals the
  variant's, table by table; otherwise the variant has no text and a
  recording of it holds no `signals.json`, so a copy never decodes
  otherwise. The fuzzer requires every variant it loads to have one.
- **The recording** holds that text as `signals.json`, with ADR-0051's
  `signal file` line and after it `plugin  <id>,<version>,<variant>`. A
  replay loading the copy reads it as an ordinary signal file (profile
  `user.<the plugin's name>`) and decodes the same values; it names no
  plugin.
- **`havne-cli stream --plugin --record`** gives the recorder the
  variant the URI's `profile=` selects, as it gives a `--signals` file.

> **Note of 2026-11-05, a variant with no one file.** A variant whose
> merged text doesn't read back to its set (past 1 MiB written out, say)
> no longer loads without a copy: the plugin is refused, naming the
> variant and the loader's reason, so every variant a plugin loads has
> one. A manifest `name` with no letter or digit, or one making a
> profile name past 63 characters, is refused at the name, since each
> variant's file takes its profile name from it.
