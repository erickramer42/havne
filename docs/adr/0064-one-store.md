# ADR-0064: One store: every import becomes a plugin

- **Status:** Accepted. Built: "Build steps" 1 to 6, the phone run
  and the removal of the earlier version's data (the appendices of
  2026-11-13 to 2026-11-18).
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
- **`requires.core`** is the least core version that reads the
  plugin's format and what its file uses, 0.8.0 at least (the
  `lineage` the wrap writes): the same input wraps to the same bytes
  under any core, so the store finds its copy again, and an older core
  refuses the plugin with words rather than half-reading it (Q6).
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

*Note (2026-11-14): as built, `wrappedFrom` is an object, each signal
file's SHA-256 to the id of the variant made from it, one file for each
variant ("the store as built" below).*

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

Amended 2026-11-17: the form's kinds gain `computed`
([ADR-0062](0062-computed-grammar.md)'s appendix of 2026-11-17); it saves
through the same `put_variant`.

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
named in the Consequences and goes to v0.9.0 with a later signal-file
format.

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
| The page | QML (`QmlTest.settings_*`, `.vehiclebook`, `.signalform`, `.phone`) | One Import's three kinds; picks and the stand-in with one and two obd2 variants; the form saving into the own variant; Save as a signal file; Copy to my plugin; Export plugin on a plugin of the user's own and on anyone else's, its share-alike note, Mine's line and the update notice; the book's format 2 and the old key untouched; every page at 384 and 320 px |
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
  variant (v0.9.0, with a later signal-file format); an OBDb plugin with a
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

## Appendix: the store as built (2026-11-13)

`app/pluginstore.{h,cpp}` and the bridge's calls over it build step 3,
and settle what Q2, Q3, Q4, Q5 and Q7 left to the build:

- **The index** is `plugins/store.json`: `{"format": 1, "plugins":
  {"<id>": {"mine", "importedFor", "wrappedFrom"}}}` and, once the
  migration has run, `"migrated": 1`. `wrappedFrom` is an object, each
  signal file's SHA-256 to the id of the variant made from it, so a
  saved address or layout maps to a variant and not only to a plugin. A
  key this version doesn't know, at the top or in a plugin's entry, is
  kept as it is. An index with nothing to say is deleted, so removing
  the last plugin still removes the folder. A plugin's entry goes when
  the plugin is removed, and when a copy that no longer loads is
  removed and its text still names its id.
- **The index is never written over when it can't be trusted.** A file
  that is there and isn't an index (unreadable, over 4 MiB, not a JSON
  object, `plugins` not an object) is moved aside whole as
  `store.json.bad-N`, the first free N from 1 to 99, and a new index is
  started; the log says so. When it can't be moved, it is left as it
  is: what is marked holds while the app runs, the answer to the user
  says the index couldn't be written, and the next change tries again.
  An index whose `format` is above 1 is read for the keys this version
  knows and never written. Each change is made to the file as it is at
  that moment (read, changed, written whole), so two running apps
  don't undo each other's facts; a change the file never got stays in
  memory and is written with the next one that succeeds.
- **The migration's mark can be lost** with an index moved aside, so
  step 4 also looks for what it wrote (the vehicle book's new key)
  before it runs again.
- **Kinds** are told apart by the file's first key, read from its text
  after any byte-order mark: `havne_plugin` is a package and
  `havne_signals` a signal file. A file starting with neither is an
  OBDb set when its object has `commands`; one past an OBDb set's
  bound is told by the word alone and refused as an OBDb set. Anything
  else is refused as none of the three. A file whose format key isn't
  first is refused here rather than by a loader.
- **A file is read once and held, then kept.** A signal file's read
  says what it would be (its name, the plugin id a wrap gives it and
  the id a variant made from it takes). An OBDb set's read holds its
  bytes for the report, which converts them as often as the year or
  the name changes. Nothing is kept until the user chooses. A refused
  keep leaves the file held, so a changed variant id or a disconnect
  can be followed by Keep again.
- **A taken id (Q2):** the wrap is asked again with `-2` to `-99` after
  the id, cut so it stays within 31 bytes with no `-` left before the
  number.
- **The same file again** finds the plugin it made: by `wrappedFrom`
  (the file's hash naming the variant the wrap would make), however
  the plugin has changed since, with words saying the plugin's version
  when it has; and by its bytes, when the index holds no such fact and
  the store holds the file's wrap, whose facts are written then. No
second plugin is made. A file
  that was only given to a plugin as a variant still makes a plugin of
  its own, and a file whose variant another file has since taken the
  place of is no longer in that plugin and makes one again.
- **`wrappedFrom` names what the plugin holds:** one file for each
  variant. A file put in a variant's place takes the earlier file's
  entry, and an entry goes with its variant (a drop, an update whose
  version lacks it, an OBDb set that replaced the conversion).
- **`importedFor` (Q3)** is the first vehicle a wrap was kept for. The
  same file kept again for another vehicle finds its plugin and moves
  nothing.
- **An OBDb set imported again (Q2)** goes to the plugin that is the
  user's own, was imported for that vehicle, is a conversion (the
  manifest's `source` is the importer's) and has that name. While that
  plugin has one variant, the new conversion takes its place under its
  id, and the answer carries the profiles it replaced so a pick of the
  earlier year can follow. Once the user has added variants, the new
  conversion joins them through the put (the lineage kept, the minor
  raised), so nothing of the user's goes. The conversion the plugin
  already holds changes nothing. Another vehicle's set of that name,
  and a file the user wrote under it, are plugins of their own. A set
  kept for no vehicle goes to no earlier conversion, one kept for no
  vehicle included: it is a plugin of its own, unless the same bytes
  were kept before, when it finds the plugin they made.
- **The variant of an OBDb plugin (Q2, Q3)** is `my<year>` only when
  the caller says the year was typed, and `base` otherwise, year 0
  included.
- **A package with the id of a plugin of the user's own (Q4):** the
  same lineage and a higher version updates in place and the answer
  names the version it updated from; a lower version is refused as
  older, naming both; the same version with other bytes is refused as
  changed apart, naming both; no lineage, or another, is refused naming
  the user's plugin. Versions compare as three numbers. Where the
  plugin isn't the user's own, a package of its id replaces it whatever
  its version, as before.
- **Facts belong to the plugin they were written for.** A plugin that
  comes in under an id with facts and no plugin loaded (the copy they
  were of is gone or no longer loads) starts with none: a package is
  someone else's until marked, and a wrap writes its own. The words on
  a copy that no longer loads say to import the plugin again, so the
  import isn't refused.
- **Put and drop (Q4)** read the kept copy, call core, and install the
  new text in the old one's place. Both are refused for a plugin that
  isn't the user's own. Core's words for the user (an ECU of the file's
  that took another id) come back as `notes`.
- **Mine, and a release, that the index can't take** hold while the
  app runs, and the next change that is written carries them. The
  answer says so: if no later change writes the index before the app
  closes, after a restart the plugin isn't marked as the user's own, or
  each import may be its vehicle's again. A forget with nothing to
  release writes nothing.
- **A vehicle forgotten** (one, or every vehicle): `importedFor` naming
  it goes from each plugin, so a vehicle the book later gives that id
  (the book numbers a new vehicle past the highest id it holds, and a
  forget frees that id) inherits no import. The plugins stay, and stay
  the user's own.
- **Picks (Q5)** are an ordered list of profiles the vehicle book
  keeps; the store answers from it. A pick is added last, a profile
  picked already changes nothing, a variant no plugin here has and a
  17th pick are refused. The row marks a pick whose variant is gone as
  missing and, on an adapter, the first pick that extends `obd2` as the
  stand-in. The list for a vehicle has the VIN's matches and the
  plugins imported for it first, each group by plugin name, then id,
  then the manifest's order.
- **Export (Q7)** writes the kept copy after checking it still hashes
  to its name. A copy changed since is refused.
- **Export's note (Q7)** names each variant whose source's license is
  Creative Commons' share-alike (core's `ecuio_signal_file_source` on
  the variant's written file), one line each, in the words
  `havne-cli plugins check` gives, so the page and the CLI say the same.
  A variant under an attribution-only license, or none, is not named.
- **A variant's signal file (Q7)** is core's text for the variant
  (`ecuio_signal_file_text`): its files merged, a notes line naming the
  plugin, its version and the variant, its source kept. Save as a
  signal file writes it byte for byte, and the form reads it.
- **Copy to my plugin (Q8)** puts a variant's signal file into a plugin
  of the user's own, a new one (the wrap, for no vehicle) or one picked
  (the put, refused for one that isn't the user's own), its source
  kept, and the answer's notes end with "The copy won't follow <name>'s
  updates to <variant>". A copy never takes a variant's place: one into
  the plugin it is in, or under a variant id the plugin already has, is
  refused, and the user names it again. The original is left as it
  was.
- **While a session decodes with a plugin,** an import, an update, a
  put, a drop and an OBDb re-import that would replace it are refused
  with nothing changed, and the answer says so (`inUse`).
- **For step 4:** `PluginStore::wrap` is the migration's wrap (run
  again, it finds each plugin); `wrappedFrom(fileSha)` gives every
  variant made from a stored file, `{ id, variant, profile, wrapped }`,
  the plugin the file made before one it was given to; `migrated()` and
  `setMigrated()` are the mark.
- **For step 5:** an answer's `replacedProfiles` beside `profiles` is
  what a vehicle's pick and a dashboard's profile name are moved by
  when an OBDb set is imported again at another year.

## Appendix: the migration as built (2026-11-14)

`app/storemigration.{h,cpp}` builds step 4. `main()` calls
`SessionBridge::migrateStores` after the earlier name's preferences are
carried over and before QML loads, and logs its lines once the Log page
listens. It settles what Q6 left to the build:

- **Two facts decide what a start does.** `vehicles/book` holding
  anything: the files are moved and the book is the user's to edit, so
  neither is done again; only the addresses and the mark are left. The
  index's `migrated`: nothing is left, and a start reads the index and
  nothing else. The book is the first commit point because an index
  moved aside (`store.json.bad-N`) loses the mark, and a second move
  would write an old book over one the user has edited since.
- **A lost index doesn't bring the `mine` facts back.** With the book
  there and the mark gone, a start rewrites the addresses (which changes
  nothing a second time) and sets the mark. Wrapping every old file
  again would bring back plugins the user has removed since.
- **A file is moved when the store names a variant made from it**
  (`wrappedFrom`), whatever this run's own wrap said, and judged once
  every file has been through the wrap. That one rule gives a vehicle's
  picks and the addresses' new profiles, so a start that only finishes
  the addresses agrees with the start that wrote the book. The profile
  is never assumed to be `plugin.<slug>.base`: an id another plugin
  holds is numbered, and the file's own plugin comes before a plugin it
  was given to as a variant. Two OBDb conversions of one name for one
  vehicle share a plugin, and the later takes the earlier one's place in
  it as a second import does: the earlier is then unmigrated, with the
  words "The plugin store holds no plugin made from it: import the file
  again" and its bytes kept. A file whose copy is gone but whose plugin
  the store holds (a stop after its wrap, the copy deleted since) is
  moved.
- **The book is carried, not judged.** Each entry keeps every key it
  has (`vinMatches`, `baseline`, one a later version adds) but `signals`
  and `plugin`, which become `picks`; `own` becomes the own file's
  profile, "" when there is none or it didn't move; a file that didn't
  move is listed under `unmigrated`, a key the entry has only then. The
  top level keeps `salt`, `picked` and any other key and gains
  `"format": 2`. What format 1's reader let through and a stricter
  reader of format 2 would refuse is settled here, so no book is lost
  whole to one odd field: an element of `signals` that names no copy by
  a SHA-256 is passed by, an element of the list that isn't an object is
  left out, and `uri` is always text, "" when it was missing or wasn't
  text. More picks than a vehicle may make are all kept.
  `tests/data/vehiclebook` holds the book in both formats for the two
  readers.
- **`importedFor`** is given for an OBDb conversion (the first vehicle
  that lists it) and for a vehicle's own file (the first vehicle whose
  form wrote it), so both sit under "For this car:". A plain file two
  vehicles share is no one vehicle's. The wrap's variant is always
  `base`: the old store never recorded that a year was typed.
- **An old book that can't be read** (text that isn't an object with a
  `list`), which an older Havne shows as no vehicles: the new book is
  empty, the old key stays, and every copy in `signals/` that loads is
  wrapped as a plugin of the user's own that no vehicle picks. A copy
  that doesn't wrap stays in `signals/` and is listed, as an entry's
  are, under the new book's own `unmigrated`, a key the book has only
  then. A saved address is renamed by the profile each copy itself
  makes, since no book says which profile a copy had. With a readable
  book only the files it names move; a copy no vehicle names is one
  whose delete never finished, and stays where it is.
- **Unmigrated bytes have their own home**, `plugins/unmigrated/<sha256>.json`,
  named by the hash the book has (a copy changed since it was kept no
  longer hashes to it), so they outlive the old folder's removal. An
  `unmigrated` record is `{ sha256, name, words }`. A copy larger than
  core reads of anything (`ECUIO_PLUGIN_PACKAGE_MAX`), or one the folder
  wouldn't take, isn't kept: its record carries `"kept": false`, the log
  counts such files, and the rest still move. A record of the book's
  own list is always `"kept": false`. The old folder's removal spares
  every copy a `"kept": false` names. `StoreMigration::unmigrated`,
  `unmigratedBytes` and `removeUnmigrated` serve the page's Save and
  Remove. An older Havne's plugin store lists only
  `*.havne-plugin.json` in `plugins/` itself, so it passes the folder
  by.
- **The addresses** (`session/uri`, each of `session/recentUris`, a
  vehicle's `uri`) change only when the profile core would read from
  them, the last `profile=`, is a moved file's. The new profile goes
  last and the other keys keep their order. An address naming an
  unmigrated file's profile, or a `user.` name no moved file has, is
  left as typed, and core's refusal says why at the connect. A recent
  list that isn't an array of text is left byte for byte. The two
  session keys are shared with an older Havne, which then shows an
  address naming `plugin.<id>.base` that it can't connect until a
  profile is picked.
- **Preferences that can't be written** leave no mark: the run says
  "The vehicle list couldn't be saved" and the next start moves the
  files again, finding each plugin it made. Addresses that can't be
  written (the book saved, the preferences refusing the next write) say
  so and leave no mark either, so a later start renames them. An index
  that can't be
  written (a later Havne's, a read-only folder) leaves the mark unset:
  each start checks again, changes nothing and says so once.
- **Two apps starting at once** both move the files. The wrap is
  deterministic and every write is whole (a package and the index by
  `QSaveFile`, one preference), so both write the same bytes.
- **The cost** is one bounded read, one wrap and one index write for
  each stored file, on the GUI thread before the first frame: 17 files
  in 120 ms on the development machine (a Release build), 10 ms under
  GCC in WSL. The phone's figure is in the phone run's sheet.

The old data, `vehicles/document` and `signals/`, is never written,
renamed or deleted by v0.8.x. Its removal (the gap named above) deletes
it only when the mark is set and `vehicles/book` holds text, and leaves
every copy the book's `"kept": false` records name.

## Appendix: the wrap in core as built (2026-11-13)

`core/src/plugin_wrap.cpp` and `ecuio/plugin.h` build step 1, and settle
what Q1, Q2 and Q4 left to the build:

- **Four calls and the free:** `ecuio_plugin_wrap`,
  `ecuio_plugin_put_variant`, `ecuio_plugin_drop_variant` and one
  accessor Q1 doesn't list, `ecuio_plugin_lineage` (the store compares
  lineages and a test reads one), with `ecuio_plugin_text_free` for the
  text the first three give. `ecuio_plugin_wrap_options_t` is as Q1
  gives it, its pointers last, so the numbers' offsets are one on every
  width (`years` at 8, `id` at 40).
- **A longer options struct:** a `struct_size` past the struct's is
  taken and what lies past the struct is never read. A field a later
  header adds there comes with a bit in `flags`, which a core that
  doesn't know the field refuses, so nothing asked is dropped unseen.
- **The detail on success:** after `ECUIO_OK` from a put,
  `ecuio_last_error_detail()` holds what the user is told (below) or
  "". A caller reads it right after the call, before another core call
  on that thread.
- **Each input goes through its own loader first,** so a refused signal
  file is refused in the signal-file loader's words and the file's own
  lines, and a refused packed plugin in the plugin loader's (a put's,
  which has two inputs, after "the plugin, "). The text written is then
  loaded before it is given; a refusal there keeps the loader's reason
  and drops the place, which is in a text the caller never saw.
- **The bytes:** the manifest's members in a fixed order
  (`havne_plugin`, `id`, `name`, `version`, `requires`, `lineage`,
  `source`, `tiers`, `match`, `ecus`, `variants`, `files`), written by
  `ecuio_json::write`. The text is written from what was read, so an
  input's line ends and spacing leave no trace in it; the lineage is the
  hash of the bytes as given, so the same file saved with other line
  ends is another lineage. One test pins a whole text, which MSVC (x64
  and x86), GCC and Clang each write.
- **The id** is the name's slug cut to 31 bytes with no `-` at its end.
  Q2's "signals" for a name with nothing left can't occur: the
  signal-file loader refuses a name with no letter or digit. A store
  that holds the id already asks again with an id of its own
  (`options.id`): the first wrap's id read back, cut and numbered.
- **A lineage** is 32 of `0-9` and `a-f`, lower case only, so two are
  compared as text. A put and a drop keep it and add none.
- **A put** raises the minor and sets `requires.core` to this core's
  version, since the file put in may use what an older core doesn't
  read; a drop raises the minor and leaves `requires.core`, since it
  adds nothing to read. A minor at 65535 refuses both. A put of a
  variant the plugin has keeps its place and its `years`. With no
  variant named, the id is the file's name as a slug cut to 24 bytes.
  The data file is `data/<variant>.json`, or `data/<variant>-2.json`
  and on while a file that stays has that path.

  *Note (2026-11-16): `requires.core` is not the running core's
  version. The wrap writes the least core that reads the plugin it made;
  a put writes the higher of the plugin's own and the least core that
  reads it with the file put in, so a value its author set above that is
  kept; a drop leaves it. The loaders work the least out at the gates
  that read each feature (`core/src/least_core.h`: one row for each
  format and each feature an older core refuses or reads to another
  meaning, with its release), and a row of a release still to come is
  cut to the running core's version, so a build before that release
  takes what it wrote. A put brings a data file with a source of its
  own, which raises it to 0.8.0 unless that source is the manifest's;
  so a put into a hand-written package with no `lineage` writes less
  than 0.8.0 only when the file's source is the manifest's, and a wrap
  always writes 0.8.0 or more. `PluginWrapTest.EachRowsInputNeedsItsCore`,
  `.AWrapRequiresTheLeastCoreThatReadsIt`,
  `.APutKeepsAHigherRequiresAndRaisesALowerOne`.*

  *Note (2026-11-17): a drop judges its result as a put does. The line
  above has a drop leave `requires.core`, "since it adds nothing to
  read". It does add something: the raised version is in each variant's
  written copy (its notes), so a version one digit longer ("1.9.0" to
  "1.10.0") can carry a copy at one file's 1 MiB past it, which a core
  before 0.9.0 refuses (ADR-0065, `least_core.h`'s `kSetBytes`). And a
  stated value below what the plugin needs stayed below it through a
  drop, which is what Q1's least core exists to prevent.
  `ecuio_plugin_drop_variant` ends as `_put_variant` does: `requires.core`
  is the higher of the stated value and the least core that reads the
  result, never past this core's own version.*
- **A data file's own `source`:** the wrap moves the file's to the
  manifest; a put keeps the file's in its data file, and the loader
  writes a variant's signal file with the source of the first of its
  files that has one, the manifest's when none has. A plugin written by
  hand with a variant of several files follows the same rule.
- **A plugin's ECUs (Q4), by address:** an ECU of the file's at the rx
  and tx of one the manifest has is that ECU, under the manifest's id
  (so a file that calls the engine ECU `ecm` joins a plugin that calls
  it `pcm`); any other is added, renamed `<id>_2` (`_3` and on) when the
  manifest has its id at other addresses. The file's signals take the
  manifest's ids. An ECU that only a replaced or dropped variant's files
  read leaves the manifest; one that nothing read before the call
  stays.
- **A signal that names no ECU** reads its file's first, or with none
  the first the extended set asks. In a plugin it would fall to the
  manifest's first, so a put gives such a signal its ECU by id wherever
  that would be another one: the file's own signals, and the plugin's
  when the file brings a manifest with no ECUs its first. The extended
  set's ECU is added as `obd2` (rx 7E8, tx 7E0) when the manifest has
  none at its addresses.
- **What the user is told:** on success `ecuio_last_error_detail()`
  holds "the file's ecu 'ecm' is the plugin's 'pcm'" or "the file's ecu
  'pcm' is 'pcm_2' in the plugin, whose 'pcm' has other addresses" for
  each ECU that took another id, and is empty otherwise.
- **A put the plugin can't hold** (an ECU past what one session reads,
  two ID widths, a 33rd variant) is refused in the loader's words, and
  ends "; the file can be a plugin of its own" only when a wrap of the
  file alone loads.
- **Not built here:** a put of the same content again raises the
  version again (the caller skips a put that changes nothing).
- **A file that loads and doesn't wrap:** one at the edge of the
  signal-file loader's bounds. A packed plugin holds a data file two
  levels deeper than the file stands alone, inside the same nesting
  bound of 32, so a file nested 31 or 32 deep is refused ("nested
  deeper than"). And a variant's signal file, the text a recording
  copies and a replay reads back through the loader, is written two
  spaces a level and must itself be within the loader's 1 MiB: a file
  of 600 to 900 KB on one line can pass that written out ("past the
  limit of"). The bound stays, since a larger written file is one no
  replay or `--signals` would load. Q6's migration keeps such a file
  as unmigrated, with those words. The largest signal file measured,
  an OBDb conversion, is about 200 KB and 8 deep.

## Appendix: the one-store UI as built (2026-11-15)

Build step 5: the page's choices the questions above left to the build.

- **A pick is read at once under Default.** A pick made while the address names no profile and the Profile row reads Default puts its profile in the address; one made under a chosen profile leaves the address alone. An import that gives one variant is such a pick. A second pick that reads obd2 is read the same way; the stand-in stays the first.
- **A list keeps its row's address.** What a connect from a list keeps (the recent list, the vehicle's address, the last address) is the row's address, without the profile the page added: that profile is the pick of the vehicle picked at that moment, and kept, it would be read for the next vehicle picked and after the pick is undone. Each connect adds it again. A row that names a plugin's profile itself (typed in Settings and kept as typed) is read as it is for a vehicle that picks the plugin; for any other the profile is dropped, as the field drops it, so a connect never reads one vehicle's signals for another and remembers them as its own.
- **The stand-in on a list connect.** Home's and the Dashboard's lists compose the stand-in's profile onto an adapter's address that names none. On SocketCAN the rule applies only to an interface listed up and able to send (a virtual bus, or a controller in one-shot mode): core refuses a requesting profile on a listen-only interface, and a session that was listening would otherwise start sending requests. A listening or unlisted interface connects as it is, under its own default (raw).
- **Settings' own Connect** connects the address as the field reads: Default stays Default when chosen, with the stand-in offered beside it (Q5 unchanged).
- **A later format's book** is never written over: the page shows no vehicle from it and says why. The store works without the book.
- **An OBDb set with no vehicle picked** is imported for none, named "OBDb set".
- **Replace** reads only a signal file: the bridge's read takes the kind it may be, so a package picked by mistake is not imported.
- **A vehicle's VIN matches** (`vinMatches`, ADR-0052's appendix of 2026-11-13) are a field of format 2, kept on the vehicle its VIN identified and dropped with a plugin's replaced profiles.
- **Counts on a row** are the store's: a variant that extends obd2 reads "obd2 + N", N the signals it adds (a signal that takes the place of one of obd2's adds none).
- **A form signal's key** is made from its name at the first save and kept by every Edit, so a gauge, a layout and a condition that name the signal by key stay bound through a rename. A new signal whose name makes a key the variant has takes the next free one ("oil_temp_2"); typed with a saved one's exact name it takes that one's place, as before.
- **A signal with no `ecu`** in a file written by hand reads the file's first ECU (the loader's rule), which the form makes explicit when it reads the variant, so an Edit keeps that ECU.
- **"For this car:" is a plugin's, whole.** A plugin is the picked vehicle's when it was imported for the vehicle, when the car's VIN matches one of its variants (the lookup's VIN, the session's, or the matches kept from the vehicle's last session) or when the vehicle picks one of its variants. All of that plugin's rows then list under "For this car:", in the plugin's own order, so its variants never split across the two groups: the first pick lifts the plugin's rows once, and a second pick or a VIN that covers one variant moves nothing. The store's `forThisCar` covers the import and the VIN for one variant; the page adds the picks, since a pick is the user's own statement that the plugin is this car's, and marks the plugin's other rows with it. "Other plugins:" holds the plugins the vehicle has nothing of.
- **C3's line says how to pick.** A plugin no vehicle picks says "Not in use: press it to pick it for <vehicle>." while a vehicle is picked, with "press a variant" where the plugin has several, and "Not in use: add or pick a vehicle, then pick it here." while none is.
- **A forgotten vehicle's files that didn't move** become the book's own: Forget and Forget all move each record to the book's list, so the kept bytes stay reachable and a file leaves only through Save or Remove (Q6).
- **A book this version can't judge** is left as it is: a `format` that isn't a number is treated as a later one. A format-2 book whose `list` is missing or isn't a list reads as no vehicles and keeps its own not-moved records and the keys a later version added.
- **Into with the Variant field emptied** takes the id core makes from the file's name, and the page says, names and picks the variant of that id. **Replace...** changes a variant and picks nothing.
- **A layout file's `files`** are the picks its `key:` references need, each by the plugin's name (with its variant where the plugin has several) and the SHA-256 of the variant's written file. A load finds a pick by that hash or by the hash of a signal file the variant was made from (`wrappedFrom`), so a layout saved before the move, which names the signal file itself, is told of nothing missing once the plugin made from that file is picked. The key and its shape are unchanged, so `havne-cli layout check` reads both. A name past the file's 80 code points is cut before its variant, which stays whole. A layout names a pick's file by its content, so a variant replaced or edited since the save is another file: the load says the layout was saved with another version of that plugin, and only a plugin the vehicle doesn't pick is called missing.
- **The one list's saves go through the save picker** (as every save in the app does): Save as a signal file..., Export plugin... and a not-moved file's Save... offer the page's own name, and a save that is refused or fails removes only the empty document its own pick made.
- **A press keeps its focus** in the one list, as it did in the earlier Signals page: the rows are not remade by a pick, and the focus follows the row a first pick moves.

## Appendix: the old data's removal as built (2026-11-17)

Q6 left the old data, `vehicles/document` and `signals/`, for an older
Havne through v0.8.x. It is removed at the start after the one that
migrated: `SessionBridge::migrateStores` calls `StoreMigration::removeOld`
before the move, so only a mark an earlier start set removes anything,
and the start that moves the files keeps them where an older Havne reads
them.

- **When:** the store's mark is set, and `vehicles/book` holds a format 2
  object. A later format, a `format` that isn't a number, text that isn't
  JSON or no book removes nothing, since the records that spare a copy
  can't be read from it.
- **What stays:** every copy whose bytes are nowhere else: one a record
  names, in a vehicle's `unmigrated` or the book's own (where Forget,
  Forget all and an unreadable old book put them), with `"kept": false`
  or with no file of its hash in `plugins/unmigrated/`. It is judged by
  its name alone: a copy edited since is still its record's. It stays in
  `signals/`, where the record's Save reads it (`oldCopy`): such a copy
  is past what `plugins/unmigrated/` holds, that folder refused it, or it
  was there but wouldn't open at the move (held open elsewhere), which
  the move records as `"kept": false`. A file in `signals/` not named
  `<sha256>.json` stays too.
- **Files added after the move:** an older Havne run after the move adds
  its files to `signals/` and `vehicles/document`, which this release no
  longer reads; they go with the rest (Q6: a downgrade past v0.8.0 sees
  an empty store).
- **The order,** each step safe to stop after: `vehicles/document` first
  (a failed write stops it with nothing deleted, said in one line, tried
  again at the next start; so does the key still read after the write, as
  Windows' registry leaves it when its delete fails, which sets no error,
  and as a fallback file of a scoped `QSettings` does), so an older
  Havne in between shows no vehicles rather than vehicles whose files
  went missing; then each other copy; then the folder, when it ends
  empty. A restart finds the mark and the book again and finishes.
- **A record's Remove** still deletes the kept bytes alone; its copy in
  `signals/` goes at the next start, spared by nothing.
- **The log:** one line for each part that went, the spared copies
  counted only beside a removal; nothing when there was nothing to
  remove. Seam: `stopAfterForTesting(Step::OldBook)` and
  `(Step::OldCopy, n)`.

Amended 2026-11-20: the removal deletes a copy only when its bytes are somewhere else whatever the book says: a plugin was made from it (`wrappedFrom`) or `plugins/unmigrated/` holds them, and the copy still hashes to its name (one edited since stays, reported once as left). The book's records only add to what is spared, so no book state takes the only copy: a v0.8.0 move recorded no copy that no vehicle named, and a start after it would otherwise have deleted those. A copy in `signals/` that no vehicle of a readable book names gets a record under the book's own `unmigrated`, `"kept": false`, so Save and Remove reach it as they reach a file that didn't move. A copy whose `kept: false` record the user removes stays in `signals/` for good, and the folder with it: accepted, since its bytes are nowhere else. This amends "A record's Remove" above, whose copy no longer goes at the next start.

**Note (2026-11-21):** the removal as it stands in v0.9.0 differs from the appendix above in these ways.

- **Added files:** an older Havne run after the move adds its files to `signals/` and `vehicles/document`. The removal takes only what it can show is held elsewhere: a copy goes when a plugin was made from it or `plugins/unmigrated/` holds the very bytes it has (compared, not just the name), and it still hashes to its name. An older Havne's new copies are left in `signals/`, counted in the "left" line. The bullet "they go with the rest" is true of the `vehicles/document` key only, and only when the move read the book.
- **A record's Remove:** it deletes the kept bytes and the record. The copy in `signals/` is then left for good: no record spares it and no plugin or kept file holds its bytes, so the removal never takes it. The bullet "its copy goes at the next start, spared by nothing" is wrong.
- **What stays, judged by bytes:** a copy is no longer judged by its name alone. A kept file cut short or damaged, or a copy edited since the wrap or the keep, keeps the copy.
- **A v0.8.0 move recorded no copy that no vehicle names.** At the first removal after the move, each such copy that is nowhere else gets a record under the book's own `unmigrated` (kept false, "No vehicle lists it"), once: the preference `vehicles/unnamedRecorded` says it is done (the move of this version sets it). A record the user removes afterwards is not made again.
- **An old book the move couldn't read:** `vehicles/document` stays where it is, whole, at every start; it is removed only when the move could read and carry it. One line says so in the activity log.
- **Device names:** a plugin folder's path part ending in a space (`nul `) is refused like one ending in a dot.
- **Downgrade:** running v0.7.x after the move is unsupported. A vehicle list it writes under `vehicles/document` is not read again by a later version (the store is marked, so the move doesn't run), and the copies it leaves in `signals/` stay, unlisted. Use v0.8.0 to go back; it keeps its own book.

## Appendix: step 6 as built (2026-11-18)

`SignalStore` and the bridge calls only it served are gone:
`importSignalFile`, `importSignalText`, `signalFileText`,
`saveSignalFile`, `useSignalFiles`, `deleteUnusedSignalFiles`, `keepObdb`
and `isObdbImport`. No page called them after step 5, so nothing a user
sees changed. `previewObdb(url, ...)` and `dropObdb` stay: the plugin
store's OBDb keep (`keepObdbPlugin`) takes a set either preview holds.

- **A replay of a recording whose copy matches** decodes with the copy
  under its own name (`rec.<16 hex>`), whether or not the store still
  holds the variant: the bridge asked only the old store whether it held
  the same bytes.
- **`fileOfSet`** asks the plugin store, then the replay's own files.
- **The plugin store's entries** carry, per variant, `extends`, `counts`,
  `owns` and `replacedNames` (the names of the base's signals the variant
  replaces, in the file's order; the entry's `replaced` key is the
  version replaced).
- **The earlier version's folder** is read by the migration and by
  `oldCopy` (a not-moved file's Save), and only the removal at the start
  after the move deletes from it (the appendix "the old data's removal as
  built"). Nothing else writes there or deletes from it, so a copy a
  `"kept": false` record names stays where Save finds it
  (`BridgeTest.OneStoreRemovalAloneDeletesFromTheOldFolder`).
