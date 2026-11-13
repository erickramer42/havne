# ADR-0064: One store: every import becomes a plugin

- **Status:** Accepted. Not built: "Build steps" is the plan.
- **Date:** 2026-11-13.
- **Builds on:** [ADR-0013](0013-protocol-profiles.md) (profiles chosen
  by the URI, never inferred, section 3);
  [ADR-0027](0027-vehicle-profiles.md) (the vehicle book; the VIN kept
  as a salted hash); [ADR-0031](0031-command-line-client.md) (havne-cli);
  [ADR-0049](0049-recordings-in-core.md) (a recording names its
  profile); [ADR-0051](0051-signal-files.md) (signal files, the OBDb
  import); [ADR-0052](0052-plugin-framework.md) (plugins, the packed
  form, selection); [ADR-0053](0053-python-bindings.md) (Python);
  [ADR-0057](0057-dashboards.md) (a gauge's refs, the layout file).
- **Amends:** ADR-0051 section 4 and its appendix "the OBDb import in the
  app"; ADR-0052 section 2 (the manifest's `lineage`), sections 4 and
  5 and its appendix "a variant's recording"; ADR-0027 (the book's
  entries); ADR-0057's appendix on the
  layout file (what a file's `sha256` names). A dated note in each.

## Context

The app keeps two stores for one kind of content. A **signal file**
(ADR-0051) is imported into a vehicle: `SignalStore` keeps its copy by
its SHA-256, the vehicle book lists it under the vehicle's `signals`,
it registers as `user.<slug>`, and a file that extends `obd2` takes
Default's place on an adapter (`standIn`). A **plugin** (ADR-0052) is
imported into the app: `PluginStore` keeps the packed copy, every
variant registers as `plugin.<id>.<variant>`, and a vehicle keeps one
picked variant under `plugin`. An OBDb set is converted to a signal
file and joins the first store.

A plugin's variant is already signal files merged, and every variant
is written back out as one signal file for recordings (ADR-0052's
appendix "a variant's recording"). So the two stores hold the same
content and differ only in the paths around it, and those differences
trap users: a signal file needs no VIN and stands in for Default, while
a plugin variant reached the profile row only after a VIN read matched
it (v0.7.4 lists every variant regardless, ADR-0052's appendix of
2026-11-13). There are two Import buttons, two lists, two ways to pick,
two Save paths and two sets of rules for what a vehicle has.

What any answer must keep:

- **The signal-file format** stays the content format: a user still
  writes, checks and shares signal files, and `havne-cli --signals`
  still reads one.
- **A dashboard's refs** (`id:<n>` for a built-in or base def,
  `key:<k>` for a file's own) keep pointing at the same rows, or saved
  dashboards break (ADR-0057 section 2).
- **Selection stays a decision** (ADR-0013 section 3) and the URI names
  what decodes (ADR-0052 section 4).
- **Attribution travels** with OBDb's data (ADR-0051 section 7): the
  `source` block reaches every copy, recordings included.
- **Nothing is lost** in the move: every file a vehicle has today, its
  picks, its saved address and its dashboards come through.
- **havne-cli is a first-class front end:** the same model reaches it,
  or this ADR names the gap.

## Decision

1. **One store, the plugin store.** Every import lands there as a
   plugin: a packed plugin as it is, a bare signal file wrapped in a
   generated manifest (one variant), an OBDb set converted and then
   wrapped. `SignalStore`, its folder's role and Settings' signal-file
   section go; one **Import** takes all three kinds, told apart by
   their first key (`havne_plugin`, `havne_signals`, an OBDb set's
   `commands`).
2. **The wrap is core's** (Q1): one call turns a signal file's bytes
   into a packed plugin's text, another adds, replaces or removes a
   variant of a packed plugin's text, so the app, the CLI and Python
   wrap alike and the output always loads.
3. **A plugin is the user's own or someone else's** (Q4): the store
   records which plugins were made on this device (a wrap, the form,
   the migration) or marked "mine". Only those take a new or changed
   variant; anyone else's is never edited, and a copy of its variant
   into a plugin of the user's own stands in for editing it (Q8). A
   newer version of a plugin of the user's own, exported from another
   device, updates it in place (Q4).
4. **A vehicle keeps an ordered list of picked variants** (`picks`),
   each a button on the profile row. Every variant of every plugin is
   listed for every vehicle, the VIN's matches and the variants
   imported for that vehicle first (Q3, Q5). The first pick that
   extends `obd2` takes Default's place on an adapter (Q5).
5. **The migration runs once** at the first start of v0.8.0: every
   stored signal file becomes a plugin of the user's own, the vehicles'
   files become picks in a new book under a new key, saved addresses
   name the new profiles, and the old book and folder are left as they
   were for an older Havne (Q6).
6. **Export plugin...** on every plugin, the user's own or not, writes
   its packed copy for another device, `havne-cli` or someone else: the
   variants with their own `source` blocks, none of the store's facts
   and nothing derived from a VIN, and a note saying when OBDb's
   license makes the file share-alike (Q7).

## The questions

### Q1. Where the wrap happens

**Options:** (a) the app alone, in Qt's JSON; (b) core, a pack call the
CLI and Python share.

**Recommendation: (b), core.** A plugin's data file has rules a bare
signal file doesn't (ADR-0052's appendix "the data tier's core as
built": no `ecus` of its own, the manifest's ECUs named by id, the
manifest's `source`), so the wrap moves the file's `ecus` and `source`
into the manifest and rewrites nothing else. Core already parses and
writes this JSON (`ecuio_json::write`, the variant writer), checks every
rule, and is the only reader all three front ends share. In the app
alone the CLI would need a second writer or a gap.

The shape (names settled at the build; all additive, `plugin.h`):

```c
typedef struct ecuio_plugin_wrap_options {
    uint32_t struct_size;
    uint32_t flags;           /* none yet; zero */
    uint32_t years[2];        /* a match's model years, 0 0 for no match */
    uint32_t reserved[6];
    const char* id;           /* NULL: from the file's name (Q2) */
    const char* variant;      /* NULL: "base" */
    const char* wmi;          /* comma-separated manufacturer codes, NULL for none */
} ecuio_plugin_wrap_options_t;

/* A signal file's bytes -> a packed plugin's text (one variant). */
ecuio_status_t ecuio_plugin_wrap(const char* file, size_t length,
                                 const ecuio_plugin_wrap_options_t* options,
                                 char** out, size_t* out_length);

/* A packed plugin's text with variant `variant` added, or replaced when
   it is there, from a signal file's bytes; the version's minor raised. */
ecuio_status_t ecuio_plugin_put_variant(const char* packed, size_t length,
                                        const char* variant,
                                        const char* file, size_t file_length,
                                        char** out, size_t* out_length);

/* The same with a variant removed (never the last). */
ecuio_status_t ecuio_plugin_drop_variant(const char* packed, size_t length,
                                         const char* variant,
                                         char** out, size_t* out_length);

void ecuio_plugin_text_free(char* text);
```

- **Every output is loaded before it is returned** (`ecuio_plugin_load`
  on the text), so a wrap never hands out a plugin core would refuse;
  a refusal's words are the loader's, as an import's are.
- **The output is deterministic:** the same input gives the same bytes
  (no time, no random id), so the store's copy, named by its SHA-256,
  is found again on a second wrap, which the migration's idempotence
  needs (Q6).
- **A data file's own `source` wins** for the variant written out from
  it (ADR-0052's appendix "a variant's recording" takes the manifest's
  today): a variant added from a file with its own `source` keeps that
  file's attribution in every recording, so an OBDb file's CC BY-SA
  block can't be lost by sharing a plugin with a hand-written one.
- **`requires.core`** is the running core's version: what the wrap
  writes is what this core reads, and an older core refuses it with
  words rather than half-reading it (Q6).
- **`lineage`**, a new optional manifest key (ADR-0052 section 2): 32
  hex digits, the start of the SHA-256 of the bytes the plugin was
  first made from (the wrapped signal file). The wrap writes it;
  `put_variant` and `drop_variant` keep it and never add one. It tells
  a later version of the same plugin from another plugin that reuses
  its id (Q4), names no person, device or vehicle, and is
  deterministic like the rest of the output.

### Q2. The generated id, name and version; an id that clashes

**Recommendation:**

- **id:** the file's `name` made a slug by `user.<slug>`'s rule
  (lower-case letters and digits, every other run one `-`), cut to 31
  bytes and trimmed of a `-` at either end; "signals" when nothing is
  left. "Daily driver extras" gives `daily-driver-extras`.
- **name:** the file's `name`, unchanged.
- **variant:** `base` for a wrap; for a variant added later, the added
  file's `name` as a slug cut to 24 bytes (the variant id's bound),
  unless the user names it (Q4). An OBDb conversion for a typed model
  year is `my<year>`; one for a year decoded from a VIN, or for none,
  is `base`, so no variant id carries a VIN's year (Q7).
- **version:** `1.0.0`; each change through `put_variant` or
  `drop_variant` raises the minor and zeroes the patch (`1.1.0`). One
  rule for both, since nothing compares a plugin's own version but a
  person reading it.
- **The profile** is `plugin.<id>.<variant>`
  (`plugin.daily-driver-extras.base`), which ADR-0052's bounds keep
  within 63 characters.
- **A clash:**
  - the same bytes again: the store has the copy already, nothing new;
  - an OBDb import for the vehicle that already has an OBDb plugin of
    the importer's (`ObdbImport::isConversion` on its `source`): it
    replaces that plugin, as the file is replaced today;
  - any other id already in the store: the wrap is asked again with
    `-2`, `-3` to `-99` appended (cut so the id stays within 31), as
    `VehicleBook.obdbName` numbers names today;
  - a **package** import whose id is a plugin of the user's own: an
    update in place when it carries that plugin's `lineage` and a
    higher `version` (Q4). Any other package with that id is refused,
    with words naming that plugin, nothing changed: a package replaces
    a plugin of the same id today, and replacing the user's own with
    someone else's would lose their edits, so they remove or rename
    theirs first.

### Q3. The OBDb `match`

**What OBDb's file holds:** no make and no model-year range. The make
is in the repository's name (`OBDb/<Make>-<Model>`), which isn't in the
bytes a user imports; the years are per command (a command's
`filter`), not the set's. So nothing in the file gives manufacturer
codes, and the schema's `match` requires `wmi`.

**Options:** (a) no `match`; (b) the WMI and model year of the
vehicle the import was made for, from its VIN; (c) typed by the user.

**Recommendation: (a) in the app, with the import's vehicle marked;
(c) in the CLI.**

- The app writes no `match` for an OBDb plugin. Since v0.7.4 every
  variant is listed for every vehicle, so a match only orders the list.
- The store records the vehicle an import was made for (`importedFor`,
  the vehicle's id, Q4's index), and that vehicle lists the plugin
  first, beside the VIN's matches, under the same caption. This gives
  the proposal the brief asks for with nothing taken from the VIN.
- (b) is left out: a manifest's `match` is shared with the plugin
  (Export, Q7), and the manufacturer code and model year of the user's
  own car in a file they pass on narrows the car, which ADR-0027
  section 3 keeps out of everything stored.
- The CLI's `signals import obdb --plugin` takes `--wmi <codes>` and
  `--years <from>-<to>` for a user who knows them; without them, no
  `match`.
- **The model year** stays as it is: the one typed or decoded at the
  import picks the commands (`year_takes`). The variant is `my<year>`
  for a typed year and `base` for a decoded one (Q2). A plugin holding
  a variant for each of OBDb's year ranges (ADR-0052 section 2's
  intent) needs the converter to report the ranges its filters use; it
  is not needed for the merge and waits for its own step in v0.9.0.

### Q4. "Add to my plugin": the UI, the version, a variant's name, whose plugin

**Whose plugin.** **Recommendation:** the store's own index, never a
manifest key. `plugins/store.json` beside the copies (the store lists
only `*.havne-plugin.json`, so an older Havne passes it by) holds, by
plugin id: `mine` (made here by a wrap, the form or the migration, or
marked by the user), `importedFor` (Q3) and `wrappedFrom` (the
SHA-256s of the signal files it was made from, Q6). A key in the
manifest would travel with a shared copy and make someone else's
plugin look like the receiver's; the index stays on the device.

A plugin that came in as a package can be marked **Mine** (one button):
a phone receiving the user's own plugin from the desktop (Q7's export)
is the case. Its line says what a later import of the same id then
does:

- **A newer version of the same plugin** (the same `lineage`, Q1, and
  a higher `version`) updates it in place, as the user's next export
  from the desktop does: the stored copy is replaced, the store's facts
  (`mine`, `importedFor`, `wrappedFrom`) are kept, and the page says
  "updated from 1.1.0 to 1.2.0". A vehicle's pick of a variant the new
  version lacks stays, shown as missing, until it is unpicked.
- **The same bytes** again change nothing.
- **The same `lineage` at the same or a lower version** with other
  bytes is refused, with words naming both versions: two devices that
  each changed the plugin are told apart by nothing else, and the user
  keeps one (a variant of the other can come across by **Save as a
  signal file...** and **Into** this plugin).
- **Any other package with that id** (no `lineage`, or another one),
  someone else's plugin that reuses the id, is refused (Q2).

A package marked Mine that has no `lineage` keeps none, so it is
updated only by removing it and importing the new version.

**The UI** (Settings' Signals page, the plugins list):

- **Import** reads the file. A bare signal file is offered **As a new
  plugin** (primary) and, when the user has plugins of their own,
  **Into** one of them (a button for each). An OBDb set shows its
  report first, as now, and is always its own plugin (its attribution,
  Q1). A package goes in as it is.
- Into a plugin: the variant's id is the file's slug (Q2) in a field
  the user may change; an id the plugin has already reads "replaces
  <variant>" before **Keep**, as the OBDb report says what it replaces
  today.
- A plugin of the user's own lists, under its variants, **Replace...**,
  **Remove** (never the last variant) and **Save as a signal file...**;
  anyone else's lists **Save as a signal file...** and **Copy to my
  plugin** (Q8). Every plugin has **Export plugin...** (Q7).
- **The version** rises by Q2's rule on each change, and the list shows
  it ("Daily driver extras 1.2.0").

**A plugin's ECUs.** The wrap moves a file's `ecus` into the manifest,
and all variants share them. A file added whose ECU has an id the
manifest has with the same addresses shares it; with other addresses,
`put_variant` renames the incoming one (`pcm_2`) and its signals'
`ecu` with it, saying so. One that breaks the manifest's rules (the
channel cap, one ID width) is refused, the words suggesting a new
plugin.

### Q5. The stand-in rule

**Recommendation:**

- On an adapter address, the **first of the vehicle's picks whose
  variant extends `obd2`** stands in for Default ("obd2 + <name>"),
  exactly as the first such file does now: Default's button stays only
  while the field reads it, and a pick made while the field reads
  Default moves the field to it.
- **Several:** the others are buttons of their own, labeled
  "obd2 + <name>" too, so each says it reads obd2's signals. The order
  is the order picked; a pick removed and picked again goes last. No
  "make this one first" control in v0.8.0: today's rule has none
  either, and the user changes it by picking again.
- Whether a variant extends `obd2` is core's answer
  (`ecuio_signal_file_extends` on `ecuio_plugin_variant_file`), as
  `extends` is read for a file today.
- **The list on the Signals page:** every variant of every plugin, the
  vehicle's picks marked (`checked`), the VIN's matches and the
  plugins imported for this vehicle first under "For this car:", the
  rest under "Other plugins:" (v0.7.4's two captions, the first
  widened). A press picks or unpicks. At most 16 picks a vehicle, so
  the profile row stays a row.

### Q6. Migration

**Recommendation:** once, at the first start of v0.8.0, before the
Settings page reads the book, in the bridge (C++, testable at the
bridge tier); the old state is only read.

The steps, each safe to run again:

1. **Read** the vehicle book from its current key (`vehicles/document`)
   and the signal store's folder.
2. **Wrap** each file the book references (`referencedFiles`), in the
   book's order, through Q1's call with Q2's id rules, and put the
   package into the plugin store as any import (its copy by its
   SHA-256), with `mine`, `wrappedFrom` and, for an OBDb conversion,
   `importedFor` the vehicle that had it. A file two vehicles share
   becomes one plugin both pick. The wrap is deterministic, so a second
   run finds each copy kept already and makes no duplicate.
3. **A file that doesn't wrap** (its copy missing, changed since, or
   refused by this core's loader) isn't dropped: the new book keeps it
   under the vehicle as `unmigrated: [{sha256, name, words}]`, shown in
   the list with its words, **Save as a signal file...** (its bytes as
   kept) and **Remove**, so the user can fix and import it again.
4. **Write the new book under a new key**, `vehicles/book` with
   `"format": 2`: each entry's `signals` and `plugin` become `picks`
   (the files' new profiles in the book's order, then the plugin pick),
   `own` becomes the own variant's profile (the form edits it, Q7),
   `vinMatches` stays. The old key is never written again.
5. **Rewrite the saved addresses:** each `profile=user.<slug>` that
   names a migrated file becomes its `plugin.<id>.base`, in the
   vehicles' `uri`, `session/uri` and `session/recentUris`. Every
   other key of an address is kept in place.
6. **Mark it done** (`plugins/store.json`'s `"migrated": 1`), last.

**Why a new key:** the book's reader drops a whole document short a
field (`VehicleBook.parse`), so an older Havne reading a book with
`picks` and no `signals` would show no vehicle at all, and could write
the empty book back. A new key leaves the old one whole.

**Nothing lost on a failed step:** every step before the mark writes
only new things (packages, the new key); a stop anywhere leaves the old
state as it was and the next start runs again from step 1, finding
what was made. A test seam stops the run after each step in turn.

**Dashboards:** nothing to carry. A wrapped variant keeps every
signal's `key` and its `extends`, so `key:<k>` and `id:<n>` refs name
the same rows (a base def replaced by name keeps its ID, ADR-0051
section 2). A layout file's `files` entries name a file by its
SHA-256; a layout saved before the merge names the old file's hash,
which `wrappedFrom` maps to its plugin, so the load's "missing" note
stays right. A layout saved after names the variant's written file's
hash, the one recordings name.

**Recordings:** nothing to carry. An old recording names
`user.<slug>` and holds its own `signals.json`; its replay loads that
copy as it does for any file the store doesn't hold (ADR-0051's
appendix "a replay reads with its recording's copy"), so it decodes as
recorded.

**What an older Havne then sees** (v0.7.4 run on a migrated machine):
its own book and its signal files, as they were at the migration, and
the wrapped plugins in the plugin folder, refused with words since
their `requires.core` is 0.8.0 (Q1). It loses nothing, and doesn't show
each file twice. Changes made in v0.8.0 aren't there, and changes made
in the older Havne after the migration don't come back on the next
v0.8.0 start (the mark is set): the CHANGELOG says so.

**The old data** stays through v0.8.x and is removed by a later
release's start (v0.9.0), after which a downgrade past v0.8.0
sees an empty store.

### Q7. Save and export after the merge

**Recommendation:**

1. **Save as a signal file...** on any variant writes its one signal
   file (`ecuio_plugin_variant_file`'s text: the manifest's ECUs and
   `source` folded back in, a `notes` line naming the plugin, its
   version and variant). It loads anywhere a signal file does
   (`havne-cli --signals`, another app), under `user.<the plugin's
   name>`. This replaces today's Save, which copies the stored bytes.
2. **Export plugin...** is offered on every plugin, the user's own or
   anyone else's, and writes `<id>.havne-plugin.json` through the
   app's save dialog, for another device, `havne-cli --plugin` or
   another person.
   - **What it writes:** the store's packed copy, byte for byte, so the
     export of a plugin and the file it came in as are the same file,
     and it loads wherever the stored copy does. No core call is
     needed: the store already holds the packed text.
   - **Each variant keeps its own `source`** (Q1's rule): a plugin
     holding a hand-written variant and an OBDb one exports both
     blocks, and each variant's recordings name its own.
   - **Share-alike.** When any variant's `source.license` is a CC
     BY-SA license (OBDb's `CC-BY-SA-4.0`), the export is Adapted
     Material shared (ADR-0051 section 7), and the note shown with
     Export, before the save dialog, says so: the file holds OBDb data
     under CC BY-SA 4.0, and whoever passes it on keeps that license
     and each variant's `source`. The page reads each variant's
     `source` from its written file (`ecuio_plugin_variant_file`), as
     `havne-cli plugins check` does (Q9), so both say the same.
   - **What doesn't travel:** none of `plugins/store.json`'s facts
     (`mine`, `importedFor`, `wrappedFrom`): they live in the store's
     index, never in a package (Q4). Nothing derived from a VIN either:
     the wrap writes no `match` from one (Q3) and no variant id from
     its year (Q2), and a VIN, its hash and a vehicle's `vinMatches`
     stay in the vehicle book. The plugin's `lineage` (Q1) travels; it
     is a hash of the file the plugin was made from.
   - **On another device** the export imports as any package (Q4): as
     someone else's plugin, which the user may mark Mine. Once it is
     Mine there, importing the next export of the same plugin (its
     `lineage`, a higher `version`) updates it in place; a third
     party's package that reuses its id is still refused (Q2, Q4).
3. **The form** (Add a signal, ADR-0051's appendix) edits the vehicle's
   own variant: it reads the variant's signal-file text (`SignalForm`
   parses it as today), and a save hands the new text to
   `put_variant` on that plugin. A vehicle with no own variant gets a
   plugin of its own on the first save, named "<label> signals". So the
   form's code keeps its document; only where the text goes changes.
4. An unmigrated file (Q6) saves its kept bytes, as now.

### Q8. A plugin extending another plugin's variant

**Options:** (a) now: a data file's `extends` names
`plugin.<id>.<variant>`; (b) later, with a copy in v0.8.0.

**Recommendation: (b).** (a) needs a format change (format 1's
`extends` takes `obd2` alone, ADR-0051 section 2), an order to load
plugins in, and a rule for when the base's next version drops a key
the extension uses (refused at load, a whole plugin lost to someone
else's update). That is its own design.

v0.8.0 offers **Copy to my plugin** on someone else's variant: its
written signal file goes through `put_variant` into a plugin of the
user's own (new or picked), its `source` kept (Q1), with a line saying
the copy won't follow the original's updates. The user then edits the
copy with the form. The gap (an extension that follows its base) is
named in the Consequences and goes to v0.9.0 with signal-file format
2.

### Q9. The CLI

**Recommendation:** the same wrap, no store.

- havne-cli keeps reading only the paths it is given (ADR-0052
  section 4: nothing loads that the user didn't name), so it has no
  store to merge. That is the named gap: the CLI can't list the app's
  plugins; a user passes a plugin the app exported (Q7) or the store's
  copy by its path. The CLI needs no export of its own: every command
  below writes a package to `--out`, and none of the store's facts
  exist in the CLI.
- New commands, each writing a new file, never changing its input:
  - `havne-cli plugins wrap <signal-file> --out <pkg> [--id <id>]
    [--variant <id>]`;
  - `havne-cli plugins put <pkg> <signal-file> --variant <id> --out
    <pkg>` (add or replace) and `plugins drop <pkg> --variant <id>
    --out <pkg>`;
  - `havne-cli signals import obdb <set> --plugin --out <pkg>
    [--wmi <codes>] [--years <from>-<to>]` beside today's signal-file
    output.
- `--signals <file>` stays as it is (a signal file is still the
  content format, and bench sheets name `user.` profiles); so do
  `--plugin`, `plugins list`, `check` and `match`. `plugins check`
  adds each variant's `source` license and, for a CC BY-SA one, the
  share-alike line Q7's export note gives.
- Exits as ADR-0052's appendix gives: a refused input is 3, a usage
  error the usage code.
- Python (ADR-0053) gains `havne.Plugin.wrap`, `.put_variant` and
  `.drop_variant` over the same calls.

### Q10. The docs and the tests

**Docs** (the docs commit of each step, from drafts beside the work):

- `docs/signal-files.md`: "Where Havne keeps them" becomes the plugin
  store; the form, Save and Read once in plugin terms.
- `plugins/README.md`: the one Import, picks, Mine, Copy to my plugin,
  Export plugin and the update of a plugin of the user's own, the
  manifest's `lineage`, the CLI's new commands.
- `docs/user-guide.md`: Settings' Signals page as one list.
- `docs/architecture.md`: one store in the app's map.
- `docs/testing.md`: the rows below.
- ADR notes: this ADR's amendments (0051, 0052, 0027, 0057).
- STATUS.md, CHANGELOG.md (Changed: one Import, one list; the migration
  and what an older Havne sees).
- Rules: `bridge.md` (the stores, `fileOfSet`'s order without the
  signal store), `qml.md` (the Signals page, the book's `picks`),
  `cli.md` (the new commands), `core.md` (the wrap calls).

**Tests, by piece:**

| Piece | Tier | Cases |
|---|---|---|
| The wrap calls | Unit (`test_core`, the plugin suite) | A file wraps and loads with the same set as the file alone (table by table) and the same keys; the ECUs and `source` moved; deterministic bytes; id and clash rules; `put_variant` adds, replaces and raises the version; an ECU renamed; `drop_variant` never the last; a data file's own `source` reaches the written variant; the `lineage` written by the wrap, kept by `put_variant` and `drop_variant`, never added, and refused when it isn't 32 hex digits; every refusal's words |
| ABI | `test_c_abi.c`, abidiff | The struct's offsets on 32- and 64-bit, additive only |
| Fuzz | `fuzz_plugin_manifest` (and the signal-file corpus through the wrap) | Every file the signal-file fuzzer accepts wraps to a plugin that loads |
| CLI | `test_cli` goldens | `plugins wrap`, `put`, `drop`, `signals import obdb --plugin`; `plugins check`'s license line, share-alike for an OBDb variant and plain for a hand-written one; the wrapped plugin through `plugins check --on` on CI's vcan |
| Python | `python/tests` | The three calls |
| Export and update | Bridge (`BridgeTest.OneStore*`) | The export's bytes equal the store's copy for a plugin of the user's own and for anyone else's; with a vehicle that has a VIN, `vinMatches` and an OBDb import at a decoded year, the bytes hold no VIN, its hash, a `match`, a `my<year>` variant or any `store.json` fact; the share-alike note for a plugin with an OBDb variant and none without; the round trip over two settings folders (export, import, mark Mine, a newer export updates in place with the index facts kept and a dropped variant's pick shown missing); the same bytes change nothing; the same `lineage` at an equal or lower version refused; a third party's package reusing a Mine id, with no `lineage` or another, refused |
| Store and migration | Bridge (`test_bridge`, `BridgeTest.Plugin*`, `BridgeTest.OneStore*`) | Each import kind; Mine and the package refusal; the index; the migration end to end on a settings folder of today's shape, stopped after each step and run again, giving one result; a file that doesn't wrap kept as unmigrated; the URIs rewritten and every other key kept |
| The page | QML (`QmlTest.settings`, `.vehiclebook`, `.signalform`, `.phone`) | One Import's three kinds; picks and the stand-in with one and two obd2 variants; the form saving into the own variant; Save as a signal file; Copy to my plugin; Export plugin on a plugin of the user's own and on anyone else's, its share-alike note, Mine's line and the update notice; the book's format 2 and the old key untouched; every page at 384 and 320 px |
| A dashboard across the move | QML (`QmlTest.dashboard`) | A layout bound to `key:` refs of a file before the migration binds the same rows after; an old layout file's `files` are found |
| The app on screen | `run-havne` smoke | Import each kind, pick, export a plugin and import the file again, connect `mock://` |
| An older Havne | Desk run sheet | v0.7.4 on a migrated settings folder: its vehicles and files as before, the plugins refused with words |
| The phone | Phone run sheet | Each kind imported through Android's picker; the migration of a phone with files stored; a plugin exported on the desktop, imported and marked Mine on the phone, then its next version imported and updated in place |

Each new test is mutation-checked (the clash rule, the stand-in's
first pick, the migration's mark written last, the URI rewrite, the
update's `lineage` and version tests).

## Consequences

- One Import, one list, one way to pick: a signal file, an OBDb set and
  a package all end as plugins, and a vehicle's picks are profile
  buttons whatever they came from.
- A vehicle no longer owns its files: they are the app's, so a file
  imported for one car is offered for every car, the one it was
  imported for first.
- Profiles named `user.<slug>` leave the app (they remain the CLI's for
  `--signals` and a replay's for a recording's copy); saved addresses
  are rewritten once, and an address typed from an old bench sheet
  with `profile=user.<slug>` no longer connects in the app.
- Core gains three additive calls, one optional manifest key
  (`lineage`) and one rule change (a data file's `source` wins for its
  variant), so the CLI and Python wrap as the app does.
- Any plugin can be exported and carried to another device, where a plugin
  of the user's own updates in place from its next export. The file
  carries each variant's attribution and its share-alike terms, and nothing of
  the store's index or the vehicle's VIN. An OBDb import whose year
  came from a VIN is named `base`, not `my<year>`, so two such imports
  for different years are told apart by their plugins' names only.
- Updating by `lineage` trusts the hash, not a signature: a package
  made to carry another plugin's `lineage` updates it. The refusal is
  against an accidental clash of ids, not an attacker; a plugin is
  data only (ADR-0052), and the user sees the version change.
- An older Havne sees its pre-migration state; work done in v0.8.0 is
  not visible to it, and its later work doesn't come forward.
- **Gaps, each with its release:** a plugin extending another's
  variant (v0.9.0, with signal-file format 2); an OBDb plugin with a
  variant for each of its year ranges (v0.9.0); the old store's
  removal (v0.9.0); the CLI has no store by design and stays without
  one.

## Build steps

Each a branch, in order; the first two can run beside the app's.

1. **`feat/v080-plugin-wrap` (core):** the three calls and the free,
   the data-file `source` rule, `requires.core`, the manifest's
   `lineage` (written by the wrap, kept by put and drop, checked by the
   loader); unit, ABI, fuzz; `core.md` draft. A full verify (core).
2. **`feat/v080-plugin-wrap-cli` (CLI, Python):** `plugins wrap`,
   `put`, `drop`, `signals import obdb --plugin --wmi --years`;
   `plugins check`'s license and share-alike line; Python's three;
   goldens, the vcan check; `cli.md` draft.
3. **`feat/v080-one-store` (bridge):** `PluginStore` takes all three
   kinds through the wrap, `store.json` (`mine`, `importedFor`,
   `wrappedFrom`), put, drop, Mine, the package refusal and the update
   in place by `lineage` and version; the export's bytes and its
   share-alike test; an OBDb variant named by a typed year only;
   `fileOfSet` without the signal store; bridge tests; `bridge.md`
   draft.
4. **`feat/v080-one-store-migration` (bridge):** the migration and its
   step seam, the new book key, the URI rewrite; bridge tests; the
   older-Havne desk run sheet.
5. **`feat/v080-one-store-ui` (QML):** one Import, the list with picks,
   Mine and its line, the update notice, Copy to my plugin, Save as a
   signal file, Export plugin on every plugin with its share-alike
   note, the stand-in over picks, the form into the own variant;
   `VehicleBook` format 2; QML tests and qmllint; a `run-havne` smoke;
   `qml.md` draft.
6. **`refactor/v080-signal-store-out` (app):** `SignalStore` and its
   bridge calls removed (`useSignalFiles`, `importSignalFile`,
   `importSignalText`, `deleteUnusedSignalFiles`, `saveSignalFile`),
   the migration's reader kept; a full verify.
7. **The phone run and the docs commit:** the phone run sheet (with
   the desktop's export imported and updated on the phone), then the
   docs listed in Q10 and this ADR's amendments.

## Alternatives considered

- **The wrap in the app alone:** less core work, but the CLI and Python
  would get a second writer or none, and the output's checks would be
  Qt's, not the loader's.
- **Keep two stores, one list:** a merged view over both stores; the
  two Save paths, the two pick rules and the vehicle-owned files stay,
  which is what trapped users.
- **"Mine" as a manifest key:** simple to read, but it travels with a
  shared copy and marks someone else's plugin as the receiver's.
- **Updating a plugin of the user's own by id and version alone:** no
  new key, but a third party's package that reuses the id at a higher
  version would replace the user's plugin and lose their edits.
- **Asking at each import whether a package is the user's own:** no
  new key, but the question has no answer the app can check, and a
  wrong press loses the user's edits.
- **Export rewritten without the parts that came from this device:**
  the store's facts never enter a package and the wrap writes nothing
  from a VIN, so there is nothing to strip, and a byte-for-byte copy
  keeps the export and the stored file the same.
- **Rewrite the book in place:** one key, but an older Havne reads a
  book it doesn't understand as empty and can write that back.
- **`match` from the vehicle's VIN:** the variant proposed by the VIN
  at once, but the manufacturer code and year of the user's car would
  travel in every plugin they share.
- **Extending another plugin now:** the user's edits follow the
  original's updates, at the cost of a format change and a load order
  in v0.8.0, and of a plugin refused whole when the base drops a key.
- **Keep the `user.<slug>` profile for a wrapped file:** old addresses
  and bench sheets would still connect, but one profile name would mean
  a signal file in the CLI and a plugin variant in the app, and a
  plugin's profile would no longer say which plugin decodes.
