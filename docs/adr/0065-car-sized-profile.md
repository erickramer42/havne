# ADR-0065: A car-sized profile: a set defines many signals, a session asks few, and a vehicle's plugins are read together

- **Status:** Accepted. Built: the default pick (section 2), in v0.8.0
  (ADR-0056's appendix "a front end's default pick", ADR-0051's appendix
  "1024 requests"). As of 2026-11-20, v0.9.0 also builds steps 1 to 3
  (the layout and constants, tables from the set, keys and recording
  format 4), the simulated car of step 6 and the hold of the appendix
  below. Not built, v0.9.1's: step 4 (the merged set, `needs` and
  signal-file format 3), step 5 (the pick opens modules), step 7 (a
  vehicle's plugins read together) and step 8 (faster), and the
  simulated car's `vcan` cases.
- **Date:** 2026-11-15.
- **Builds on:** [ADR-0013](0013-protocol-profiles.md) (profiles chosen
  by the URI); [ADR-0015](0015-request-engine.md) (requests derived
  from a set, the rotation, channels);
  [ADR-0019](0019-session-info.md) (the session's info);
  [ADR-0042](0042-reply-timing.md) (reply timing's tables);
  [ADR-0046](0046-simulated-ecu.md) (the simulated ECU);
  [ADR-0049](0049-recordings-in-core.md) (recordings);
  [ADR-0051](0051-signal-files.md) (signal files);
  [ADR-0052](0052-plugin-framework.md) (plugins, a variant's merged
  files); [ADR-0056](0056-analysis-log.md) (the focus);
  [ADR-0057](0057-dashboards.md) (a gauge's references);
  [ADR-0060](0060-extended-session-reads.md) (session control);
  [ADR-0062](0062-computed-grammar.md) (computed signals);
  [ADR-0063](0063-conditions.md) (conditions);
  [ADR-0064](0064-one-store.md) (one store, a vehicle's picks).
- **Amends, each by a dated note when built:** ADR-0015's limits
  (requests for each set, channels for each set); ADR-0019 (the info's
  channels); ADR-0046 (the nodes, faults for each module); ADR-0049
  (recording format 4: a value is written under its signal's key);
  ADR-0051 section 6 ("`ECUIO_MAX_CHANNELS` stays 4 until a real car
  needs more"), its bounds and its appendix "boost in the obd2 set" (a
  base def replaced by name and one name for each signal are format 1
  and 2's rules), with signal-file format 3; ADR-0052 sections 2
  and 4 (the ECU limit, a variant's written copy, a profile that names
  several plugins); ADR-0057 section 2 (a `key:` reference reaches a
  replaced signal); ADR-0064 Q5 (a vehicle's picks are read together;
  the stand-in rule goes) and Q8 (the gap "a plugin extending another
  plugin's variant"); ADR-0031, ADR-0054 and ADR-0056 (a signal named
  by its key in `--signal` and in an export's columns).

## Context

Three different numbers are one number in the code:

- **What a set defines.** `ECUIO_PROFILE_MAX_REQUESTS` bounds the
  requests a set's defs derive, and sizes the engine's table, reply
  timing's tables and the recorder's totals as fixed arrays.
- **What a session asks.** A session asks every request of its set, in
  rotation, unless a focus narrows it (ADR-0056).
- **How many modules it talks to.** `ECUIO_MAX_CHANNELS` is 4, in the
  set, the engine, every transport and the session's info.

A real car outgrows all three at once. On a test car with 18 modules
answering, one module alone answers about 230 readable identifiers and
a second about 94; the `obd2` base adds about 90 requests. Fourteen of
those modules have no place in a profile at all.

Raising the constants doesn't answer it. A session that asks everything
it defines pays for the set's size in bus time: at 10 ms for each
request a table of 1024 is about 10 s for each sweep, and through an
ELM327 at 50 ms about 50 s. Nobody watches a thousand signals; the
analysis log showed that a short rotation is what makes a value useful.

A second limit is one of shape. A vehicle picks plugin variants
(ADR-0064), and a session reads exactly one of them. A user with a
plugin of their own and an OBDb import of the same car reads one or the
other, or merges the files by hand, and an import that holds more than
four modules is refused ("past the 4 ECUs one session reads"). Signals
from every source should work together, come in through one Import, and
stay separate for each car.

## Decision

### 1. The rule

**A set may define far more than a session asks.** Defining a signal
costs memory, and is bounded only by ceilings that keep a bad file from
exhausting it. Asking costs bus time, and is bounded by the pick.
Nothing a set defines is asked until something picks it.

The module is the unit: a car is one merged set (one ID space, so
computed signals across modules, gauges, the focus and a recording's
one `signals.json` keep working), written as a data file for each
module, with limits counted for each module. A vehicle's picked plugins
make one such set together (section 9).

### 2. The pick

A session asks the union of:

1. the set's plot-ranked signals (`plot_rank` above 0);
2. the signals the dashboard's gauges read (the current form's
   document, every page);
3. the signals the user ticked, kept for each vehicle by key
   (section 9);
4. the signals an attached condition reads (section 8).

A set that ranks no signal is asked whole: a `uds-generic` session,
whose address already names its DIDs, and a hand-written file with no
ranks. One case is new with large sets: a set that ranks nothing and
derives more requests than a session may ask (section 3's pool) is
asked for nothing until something picks a signal, and the Log says so.

The mechanism is the focus (ADR-0056 section 2): an unpicked request is
parked, in the table and not refused, and comes back when picked. The
pick is set before the session starts, so a large set never runs one
whole sweep first, and again whenever a part of it changes. The
supported-PID walk, the DID scan, identification and diagnostics run
outside the pick. An analysis log narrows the pick to the ticked
signals while it runs and returns to it when it stops.

Rules 1 to 3 are a front end's to build, since core can't see
dashboards or ticks; rule 4 is core's, so no front end can forget it.
`havne-cli` asks what it is told, and `--pick ranked` gives it rules 1
and 4 with the signals it names; it has no dashboards or vehicles,
which is the gap between the two front ends.

What a user sees: a signal outside the pick reads "Not asked", distinct
from "not sent by this ECU" (the ECU doesn't have it), "Refused" (it
answered no) and "waiting for data" (asked, no answer yet). An unpicked
signal was never asked, so "Not asked" says nothing about whether the
ECU has it. A recording holds what was asked; its header names the pick
at the attach in a `pick` line, and each change is a timed note.

### 3. Tables sized from the set, and what stays bounded

At create, a session sizes its tables from its set. Nothing is
allocated on the worker afterwards.

| Table | Fixed size | Sized from |
|---|---|---|
| The engine's requests (16 bytes each) | an array of the cap | the set's derived requests, plus room for each DID its scan can find |
| The focus's row marks (`ecuio_take_focus`'s `keep`, one byte for each request, on the worker's stack) | an array of the cap | the engine's table, taken at create |
| Maps with one byte for each def (the focus's picks, what is askable, what is asked) | already sized from the set | unchanged |
| Reply timing's histograms (about 1.5 KB each) and the public rows (80 bytes each) | arrays of the cap | **the most a session asks, not what it defines** (below) |
| The recorder's totals, `havne-cli`'s snapshot totals | arrays of the cap | the same |
| A module's state: the engine's channel (about 2.3 KB) and the session's three reports (152, 1184 and 552 bytes) | arrays of `ECUIO_MAX_CHANNELS` | the set's modules; each transport's reassembly state beside them |

**`ECUIO_PROFILE_MAX_REQUESTS` (1024) is the most requests a session
has in rotation over its life, and doesn't bound what a set defines.**
Reply timing keeps a pool of that many rows, taken at create; a request
takes one the first time it enters the rotation and keeps it. A pick
that would pass the pool is refused in words, with nothing changed: a
rotation that long is already past any use. This keeps reply timing's
memory where it is (about 1.5 MB for a session that asks) however
large the set, keeps `ecuio_session_get_timing`'s contract and every
caller's array, and makes the constant mean what its cost is: time.

What a set defines keeps ceilings, as guards with words against a bad
file, not as budgets (the test car's richest module, about 230
identifiers, is far under them):

- **requests for each module:** 1024 (`ECUIO_MODULE_MAX_REQUESTS`);
- **modules for each set:** section 5's ceiling;
- **a data file:** 1024 signals and 1 MiB, and 64 files for each
  variant, unchanged;
- **a set built from several files** (a variant's files merged, a
  vehicle's plugins merged): 16,384 signals of its own and a written
  copy of 4 MiB. A variant's merged list is held to one file's 1024
  signals and its written copy to one file's 1 MiB, which a whole car
  passes, so a set gets bounds of its own. `ecuio_signal_file_load`
  reads up to the set's bounds, since it reads a recording's
  `signals.json`; an import, the wrap and a plugin's data file hold
  what they take to a data file's. The ID band for a file's own
  signals (from 30000) holds 35,535, above the ceiling.

At 16 bytes for each request and 120 for each def, a set of 4,000
signals over 18 modules costs well under a megabyte beyond its own
file, and 18 modules' state about 80 KB.

### 4. The pick drives the rotation

Each module's channel rotates through its own picked requests, one
request outstanding on it at a time (ADR-0015 section 1), so a module's
sweep time is its picks times its reply time, and adding a signal on
one module doesn't slow another where the adapter talks to them side by
side. Picked requests run in one rate class: a kept request's `slow` is
0, as the focus is built, so under the pick `obd2`'s ranked signals run
in one class; the two classes apply when a set is asked whole. Packing
gathers what is left. The minimum time between sweeps (ADR-0056's
appendix) applies to the pick as to a log.

A front end shows the cost where the choice is made: the sweep's
measured interval (reply timing has it) beside the count of signals
asked, so a user who ticks forty signals sees what it did to the rate.

### 5. The pick drives the open modules

A module is an ECU the set declares: its reply and request IDs, its
rotation, its one-shots (the session entry, identification,
diagnostics, the supported-PID walk) and its reports. Every channel a
set declares is opened at create while the ceiling is 4.

**The ceiling rises to 32 modules and the pick decides which are
open.** 32 holds the test car's 18 answering modules with room;
a module's state is sized from the set (section 3), so a set of four
modules pays nothing for the ceiling. A module is opened when something
picks a signal on it and closed when nothing does. A module nothing
picks is sent nothing at all, its identification and codes included:
reading every module's identity and codes is whole-vehicle discovery's
(the release after this one), and opening every declared module at
create would put 18 modules' one-shots on the bus before the first
value. So on a large set a health report and Diagnostics show the open
modules' codes; any module the pick touches is read, the engine ECU
under `obd2`'s ranked signals among them.

**Modules don't rotate through a few slots.** The open modules are a
fixed set, opened and closed by the pick, never swapped turn by turn: a
swap would cost a filter stop and start and would lose a module's
extended session between turns (ADR-0060: it lapses without its
keep-alive). A measured adapter that holds fewer filters than a car's
picked modules reopens this choice.

What each transport allows:

| Transport | How a module is heard | What bounds the open modules |
|---|---|---|
| J2534 | One ISO 15765 channel for the session through J2534-1's v04.04 API, the one every driver on hand implements, and one flow-control filter for each module, matched on its exact reply ID (`j2534_start`) | The adapter's filter count. J2534-1 (2022) states no figure for v04.04's flow-control filters: it replaces them with logical channels, up to ten for each physical channel (section 6.9; appendix A.2.13 names the change), refused past the adapter's own maximum with `ERR_EXCEEDED_LIMIT` (section 7.3.7), and keeps exactly ten pass and block filters on the physical channel, which logical channels don't use (section 7.3.14). The OBDX Pro developer's reference gives the VX 32 CAN filters in software, which it says its J2534 driver uses (section 3.14, note 1, and section 3.14.7; the processor's own filters hold 28 at 11 bits and 8 at 29); it doesn't say how many of those the driver leaves an application. So the VX's count through J2534 is a bench run's (To measure), and no standard figure stands in for an adapter's. A filter the adapter refuses leaves that module closed, its signals "Not asked" with the reason in the Log, and the session running |
| ELM327 | One receive filter for the whole session: `AT CRA` for one module, else `AT CF` and `AT CM` with a mask of the bits the reply IDs share, which may pass IDs between them (core matches each frame against the modules anyway). A switch to another module is `AT SH` alone (and `AT CP` on 29-bit IDs); the filter isn't sent again | Nothing in the filter. Time: the adapter takes one command at a time, so every module's picks share one line, and each switch adds an `AT SH` round trip (section 11). Opening or closing a module sends the filter again |
| SocketCAN | Kernel filters, two for each module (its reply ID and its request ID, for the echo) | Nothing practical: the kernel's list is far longer than a car. One request waits for its echo at a time for the socket; replies overlap |

Two rules that hold on every transport:

- **Requests in flight.** The engine keeps one request outstanding for
  each module, and the J2534 worker sends for every module with nothing
  outstanding on each pass, so 18 open modules could put 18 requests on
  the bus at once. The session caps the requests in flight at 4, what
  has run on a car, whatever the number of open modules; the modules
  take turns for the slots. Section 11 raises it by measured steps.
- **One ID width.** A session's modules are all 11-bit or all 29-bit.

### 6. The ABI (`feat!`)

One break, made once. Each line is checked by `test_c_abi.c` on both
widths and regenerates `python/havne/_ecuio.py`.

- **`ECUIO_MAX_CHANNELS`** is 32. The set's validator, the signal
  file's "past the N ECUs one session reads", the importer's "more
  modules than a session asks" and a plugin manifest's `ecus` follow
  it.
- **The set's layout is 5.** `ecuio_channel_decl_t` grows from 8 to 16
  bytes: `rx_id`, `tx_id`, `flags` and a reserved word, both zero and
  checked by the validator with a set-level code of its own. A module
  then has room for what a later record gives it (the session its DIDs
  are read in, v0.9.0's sessions as data) with no second layout.
  `ecuio_signal_set_t` itself keeps its members and size. A set built
  by hand against layout 4 is refused by its `layout_version`, as
  ADR-0016 has it.
- **`ecuio_session_info_t` keeps its 432 bytes.** `channel_count`,
  `rx_ids` and `tx_ids` keep their meaning for the first
  `ECUIO_SESSION_INFO_MAX_CHANNELS` (4) modules, so a caller that reads
  only them never runs past an array. `module_count`, in the reserved
  room, is every module the session's set declares, and
  `ecuio_session_get_module(session, index, &decl)` gives any module's
  pair. The reports (`ecuio_session_get_channel_support`, `_identity`,
  `_diagnostics`) are read by the same index, to `module_count`, and
  the support report's flags say whether the module is open, closed
  (nothing picks it) or without a filter (the adapter took no more). A
  caller that sized arrays by 4 (the app, `havne-cli`, the Python
  package) moves to the count.
- **`ECUIO_PROFILE_MAX_REQUESTS`** keeps its name and value and bounds
  what a session asks (section 3); `ECUIO_MODULE_MAX_REQUESTS` (1024)
  is new, and `ECUIO_VALIDATE_ERR_REQUEST_COUNT` reports a module past
  it. `ECUIO_SIGNAL_SET_MAX_SIGNALS` (16,384) and
  `ECUIO_SIGNAL_SET_MAX_BYTES` (4 MiB) are a merged set's bounds.
- **`ECUIO_SIM_MAX_REPLIES`** is 66: two replies for each of the 32
  modules and the engine pair (section 10).
- **The validator takes a repeated name.** Two defs of a set may share
  a name (section 9); `ECUIO_VALIDATE_ERR_DUPLICATE_NAME` is retired
  and its value never reused. A format 1 or 2 signal file is still
  refused one by its loader, and the built-in sets keep one name for
  each signal.
- **New calls,** all additive beside the break: section 9's merge and
  what reads its result, a def's address key (section 9),
  `ecuio_session_get_module`, and the address's query key `inflight`
  (section 11).
- **What the recorder writes: format 4.** A value is written under its
  signal's key (section 9), since a name no longer picks out one
  signal:
  - `signals.csv`'s `signal` column holds the key. The header's
    `signal` line gains the key as its first field and keeps the name,
    as the label. The `focus` and `pick` lines, and their changes,
    list keys.
  - Recordings in formats 1 to 3 read as they are, by name, and need
    no conversion. A core older than this one reads a format 4
    recording as a format it doesn't know: the keys shown as names,
    each kind inferred.
  - What reads a recording follows: the reader gives each signal's key
    beside its name, a query takes either a key or a name one signal
    has, and the exports match a value to its def by key. The wide
    CSV's header gives each column's key with its name and unit, and
    `havne-cli`'s `--signal` takes a name one signal has, or a key.
  - `signals.json` is the session's set, a merged one included, up to
    a set's bound; the readers' and the exports' bound follows it.
  - One `plugin` line for each plugin read together, in the merge's
    order (section 9), beside the one `profile` line.
  - `latency.csv` names any module's reply ID (the recorder reads the
    modules through the getter, not the info's four).
  - A timed `module` line when a module opens, closes or is left
    without a filter, so a reader can tell "nothing was asked of it"
    from "it didn't answer".
  - The timing rows are the pool's: a row for each request asked, not
    for each request defined.
  - The health report from a recording lists a module on any reply ID
    the copy's set declares, not only on OBD's.

  Each line from `signals.json` down is one a format 3 reader would
  pass by; they come with format 4 all the same, so a reader meets one
  change.

A second, flagged path beside the constants of 4 would avoid the break
and leave two channel limits in the headers, one of them misleading.

### 7. A whole car as one plugin variant

A plugin's variant merges a data file for each module into one set
(ADR-0052). With this ADR the variant holds every module and every
identifier the plugin knows, and its author ranks the few signals a
driver wants first; a user ticks the rest as needed, and the ticks are
kept for the vehicle. No slicing into variants to fit a budget. Live
gains a filter by module beside its groups (ADR-0050), since a list of
thousands is found by where a signal lives as much as by what it
measures. A module's name is its ECU's `id` and reply ID; signal-file
format 3 adds an optional `name` to an ECU for a longer one
(`ecuio_signal_file_module` gives both), and the row is the UI lane's
to lay out.

### 8. A condition's signal is a pick

ADR-0063 section 7 has the engine never park a request that carries an
attached, enabled condition's input, through a computed input's chain
to the signals a reply carries. Under this ADR that is rule 4 of the
pick, and it also opens the module the signal lives on (section 5). An
alert never waits on a signal nobody asks, and the cost is those
requests in every sweep.

### 9. A vehicle's plugins are read together

**A vehicle's picks (ADR-0064) are the parts of one set.** A session
for the vehicle on an adapter reads every picked variant at once: a
plugin of the user's own, an OBDb import, a manufacturer's package and
`obd2`, with nothing merged by hand. Nothing new is stored: the book's
ordered `picks` is the list, at most 16, and `obd2` is a pick like the
others (in the set when it is picked or any picked part extends it; a
new vehicle starts with it picked).

**The merge is core's,** so the app, `havne-cli` and the Python package
merge alike. One call takes the parts in order, each a loaded signal
file or a plugin variant's (`ecuio_plugin_variant_file`), and gives an
ordinary signal file the caller frees:

```c
typedef struct ecuio_merge_options {
    uint32_t struct_size;
    uint32_t flags;        /* ECUIO_MERGE_BASE_OBD2: obd2 is the base
                              though no part extends it */
    uint32_t reserved[6];  /* zero */
} ecuio_merge_options_t;

ecuio_status_t ecuio_signal_file_merge(
    const ecuio_signal_file_t* const* parts, uint32_t count,
    const ecuio_merge_options_t* options, ecuio_signal_file_t** out);
```

(Names are settled at the build.) The result is a signal file in every
way: its set, its keys, its text. So everything that takes a file takes
it unchanged: `ecuio_profile_register`, the recorder's `signals.json`,
a replay of that recording, `key:` references, a condition's terms, a
signal's gauge. It owns copies and doesn't depend on its parts'
lifetimes. The output is deterministic (the same parts in the same
order give the same bytes). Three readers are new: the parts (each
one's profile, its plugin's id, version and variant, its `source`), the
part a def came from, and the merge's notes (what was replaced by what,
what was left out and why), which a front end shows.

**The written form is signal-file format 3.** The merged text holds a
`parts` list and each signal names its part. `ecuio_signal_file_load`
reads it back to the same set, as it does a variant's copy.

**Each part keeps its source and its license.** A part's entry in
`parts` holds its `source` block whole (ADR-0051 section 7), so in
every copy, a recording's included, a signal carries the attribution
and the license it came in with. Share-alike applies to what came in
share-alike and to nothing else: an OBDb import's signals stay CC BY-SA
4.0, with their attribution, wherever a copy goes, and its terms don't
reach the other parts, which keep their own. A merged copy is a
collection of separately marked parts, not one work adapted from OBDb's
(a reading of the license, as ADR-0051 section 7's is, not legal
advice). A signal that replaced another is its own part's. The merged
file's own `source` says only that it is a merge;
`ECUIO_SOURCE_SHARE_ALIKE` is each part's, read through the parts, and
a front end's note about passing signals on names the parts it applies
to.

**The order.** The parts are layers, in the picks' order, and a later
part's signal replaces an earlier one's: the rule a variant's files
(ADR-0052 section 2) and a file over its base (ADR-0051) already
follow. The vehicle's own variant, when picked, is merged last whatever
its place, so what the user wrote for this car always wins. The Signals
page reorders the rest.

**Modules.** Two parts' ECUs with the same reply and request IDs are
one module, whatever each calls it; the earlier part's `id` and `name`
stand. An ECU whose `id` an earlier one has with other IDs takes
`<id>_2` (as `ecuio_plugin_put_variant` renames one). Left out, each
with a note and the rest still read: a module whose reply ID an earlier
one answers with another request ID, a part of the other ID width, and
a module past the ceiling, with its signals.

**A signal is its address, not its name.** The address is where the
value is read: the module (its reply ID; a broadcast frame's ID for a
signal nothing asks), the request (the service and its PID or DID, the
def's key bytes) and the field (the start byte, the bit offset and the
length; a text field's bytes). Two signals with one address are one
signal, whatever each is called and however each scales it. Two signals
with one name and different addresses are two signals, and both keep
the name.

**The address key.** Core writes a signal's address as a key: its
module's reply ID, its request and its field, as text no author's key
can be (the exact text is settled at the build). The key is the same
for one address in every part, in every set and on every device,
whatever the signal is called and whatever else is picked, so it is
what identifies a signal wherever something is kept: the vehicle's
ticks and a recording's values (section 6). Two cases have no address
key of their own and are kept under another:

- a computed signal reads no address: its author's key, and a built-in
  set's by its fixed ID, as `id:<n>` (ADR-0057's form);
- the later of two signals at one address: its author's key. Only a
  format 1 or 2 file gives two of its own (a number and an enum over
  one byte), and a part's signal at a base def's address is one (the
  base below). A format 3 file is refused two signals of its own at one
  address, as it is two of one key.

**Signals.** Each signal of a part, in order, against what the earlier
parts gave:

1. **The same key:** it takes that signal's place in the list. A key is
   the handle its author gave it, so a part that reuses another's key
   says "this is that signal".
2. **The same address:** it takes that signal's place. Two plugins that
   define one DID therefore cost one request (the engine derives one
   for each module and request, whoever defines it), each field appears
   once, in the later part's words and scaling, and the fields only one
   of them knows are all kept.
3. **Otherwise** it is added at the end, a new signal.

A name alone never replaces. A computed signal has no address, so it
follows rule 1 or is added.

**Names are labels and may repeat.** The merge renames nothing, and a
set may hold two signals of one name (two modules' "Battery voltage",
`obd2`'s engine speed and a manufacturer's own). The set's validator
takes a repeated name (section 6), and nothing kept refers to a signal
by its name: ticks, recordings, gauges, conditions and expressions all
go by key.

A screen that shows two signals of one name adds, beside each, what
tells them apart: the module (its ECU's `name` when a file gives one,
else its `id` in upper case), "Battery voltage" with "BCM" beside one
and "PCM" beside the other; on one module, and for computed signals,
the part's name; the key when that is the same too. It is worked out
when the list is drawn, from the signals in it, and stored nowhere, so
no name a user sees depends on which plugins are picked. A front end
that takes a signal by name (`--signal`) takes a name one signal has,
and for a repeated one says which keys it could mean.

**The merge's notes** name each case in one line, with both plugins,
both names and the address in words:

- a replacement by address: "'Oil temperature' (My car signals)
  replaces 'Engine oil temp' (Example import): both read PCM 22
  `<DID>`, byte 4";
- a replacement by key, which says so when the addresses differ:
  "'Boost' (My car signals) replaces 'Boost' (Example import) by its
  key boost; it reads another address";
- a format 1 or 2 part's signal named as a base def: "'Boost' (Boost
  ratio) is read beside obd2's 'Boost': a merged set replaces no obd2
  signal";
- what was left out, and why.

**The base is never replaced.** `obd2` is the layer under every part,
and its standard signals stand whatever a part defines: a part's DID
that reads the same quantity is a signal of its own beside the base's,
under the same name when its author gave it that one. So a base def
keeps its ID, its address and its meaning in every vehicle's set, and
what reads `id:7003` reads the standard PID.

- **By address:** a part's signal at a base def's address is kept
  beside it, as in a file loaded alone, and goes under its author's
  key wherever a key is kept.
- **By name:** ADR-0051's appendix "boost in the obd2 set" lets a file
  that extends `obd2` replace a base def by naming it. That stays a
  format 1 and 2 file's rule when the file is loaded alone, so
  `examples/boost-ratio.json`, every file in use and every recording's
  `signals.json` load to the set they always did. Format 3 has no such
  rule: a format 3 file's signal named as a base def is a second signal
  of that name.
- **In a merge:** the merged set is format 3 whatever its parts are,
  so a format 1 or 2 part's signal named as a base def is added beside
  the base def, with a note. A format 3 part merged alone gives that
  part's own set.

**Keys stay stable.** A replaced signal's key becomes a second key of
the signal that replaced it: `ecuio_signal_file_find` answers either,
and `ecuio_signal_file_key` gives the replacing signal's own. A gauge,
a condition or a computed signal that named the replaced signal by
`key:` still reads, from the definition that won. A re-import keeps
keys (the importer derives them from the source's own names), so a
layout survives it. A def's ID is its place in the merged list and
means nothing outside the session and its recording, as ADR-0057 has it
for a file; nothing stored refers to one (references by `id:` name base
defs, whose IDs are fixed). The vehicle's ticks are kept by address
key, so a tick is found again whichever plugins are picked, as long as
one of them defines that address. A tick an earlier version kept by
name is taken as the signal of that name when one signal has it, and is
kept by key from then on.

**Expressions across parts.** A computed signal's inputs are resolved
over the merged set, replaced keys included. A part must still load
alone, so format 3 adds `needs`, a list of keys a file's expressions
read that another part provides. Alone, or merged with no part that
provides one, a computed signal reading a needed key is left out with a
note, and so is any signal computed from it; a key neither defined nor
needed is refused, as a typing error is. This is what lets a computed
signal the user writes read an import's signals.

**The profile's name.** A merged file's profile is `merged.<16 hex>`,
the start of the SHA-256 of its parts' profile names in merge order. It
is the same on every device and in the CLI for the same picks, holds
nothing of the vehicle, and doesn't change when a part's plugin is
updated (as `plugin.<id>.<variant>` doesn't). The front end registers
it and the address names it, so everything that takes a profile name
takes it: `Bridge.profileSignals` for Add with no session, the
simulated ECU's `profile=`, a replay. The session's Log line and the
recording's `plugin` lines say which plugins it is. When a vehicle's
picks change, the app merges again, and its saved address follows.

**What a refusal becomes.** The merge fails only when nothing is left
to read or the set's bounds are passed (section 3), naming the count.
Everything smaller is a note and a session that connects.

**Settings, Signals.** For the picked vehicle the page lists the parts
in merge order, each with its plugin's name, version, variant and its
counts of modules and signals, a press to pick or unpick, and a way to
move it; the own variant sits last and says its signals win. Under the
list: the set's totals (plugins, modules, signals, signals asked) and
the merge's notes, folded ("12 signals replaced by a later plugin", "2
left out"), each opening to its lines. Import asks nothing new: what is
imported for the vehicle is picked for it. On the adapter page the
profile row has one button for the vehicle's set beside Default; the
button for each pick goes, and a single variant is still reached by its
own profile in the address. The layout is the UI lane's.

**`havne-cli` and Python.** `--together <profile>[,<profile>...]`
merges the named profiles of the `--plugin` and `--signals` files given,
in that order, registers the result and adds its profile to an address
that names none; an address naming another is a usage error. `plugins
together` prints the parts, the totals and the notes, and with `--out`
writes the merged signal file. The Python package gains
`SignalFile.merge`. The CLI has no vehicles and no store, as before.

**ADR-0064's gap, "a plugin extending another plugin's variant".** It
is closed by reading together and needs no `extends` that names a
plugin: a part picked after another replaces the signals it redefines
and adds the rest, in an order the vehicle's picks give, and `needs`
carries an expression across parts. When the earlier plugin's next
version drops a key, the later part's signals that need it are left out
with a note and nothing is refused. **Copy to my plugin** narrows to
one signal: a copy under the same key in the user's own variant
replaces the original in the vehicle's set, and the rest of the
original keeps following its updates.

### 10. The simulated ECU as a whole car

The simulated ECU answers as many modules as the ceiling, so every rule
here is tested with no car:

- **Nodes from the set.** `profile=<name>` gives a node for each of the
  set's channels, to 32, each on its own request and reply IDs with its
  own ISO-TP receiver, sender and reply under way, allocated from the
  set's count. The engine pair's node answers Service 01 as before.
- **`0x7DF` is answered by every node** that has an answer, each on its
  own reply ID, refusals unsaid; a segmented reply's Flow Control comes
  on that node's request ID.
- **Faults for each module.** Each node counts its own faults. A fault
  takes an optional `@<reply ID>` (`faults=late:3@7EA`): on that module
  alone; without it, on every module, each on its own count.
- **`ecuio_sim_ask`** gives up to `ECUIO_SIM_MAX_REPLIES` (66) replies
  to one functional request; a caller still passes any capacity and
  reads the count.
- **A made-up whole-car plugin** in `plugins/`, of 18 modules with
  made-up addresses and identifiers and a few ranked signals, is what
  CI reads whole on `vcan`, and what `plugins check` runs.

### 11. Faster after the safe defaults

The defaults above are what has run on a car. Two levers raise a
sweep's rate past them, and each is planned with the measurement that
moves it.

**What stays one.** A module has one request outstanding (ADR-0015
section 1: ISO 15765 physical addressing). A second request to the same
module is never the lever; the lever is how many modules have one out
at once, and on an ELM327, how often the line switches module.

**The engine's shape,** so a raise changes a number and a policy, never
the tables:

- **A slot for each module:** the module's outstanding request, its
  send time and its deadline, in the module's own state, sized from the
  set.
- **A session-wide cap as a parameter:** the engine holds the cap and
  the count of slots taken; a module's turn to send (a rotation request
  or a one-shot alike) waits while the count is at the cap. The engine,
  not each worker, picks which module takes a free slot: the one whose
  due request has waited longest, so no module starves however the cap
  compares with the open modules. The J2534, SocketCAN and ELM327
  workers ask the engine for the next module instead of looping over
  the channels themselves.
- **The transport's ceiling** as an argument at attach: the modules
  J2534 holds a filter for, the open modules on SocketCAN, 1 on ELM327
  (one command on the line).
- **Counters the policy reads,** kept for each module from the first
  build: replies lost (a wait expired with nothing), `7F xx 21` (busy,
  repeat request), `7F xx 78` (pending), replies later than P2 (50 ms,
  ADR-0015 section 1) by reply timing's histogram (ADR-0042), and for
  the session the frames flagged `ECUIO_FRAME_FLAG_OVERFLOW`. Only the
  policy that reads them is added later.

**Requests in flight, raised by measured steps (J2534 and SocketCAN):**

- **Start** at the default cap, 4, or at the open modules if fewer.
- **Raise** by one slot after a clean stretch: every open module has
  completed whole sweeps with no lost reply, no busy or pending answer,
  no reply past P2 and no overflowed frame since the last change. The
  stretch is counted in sweeps of the slowest open module, not in
  seconds, so it means the same at any pick. The cap never passes the
  transport's ceiling or the open modules.
- **Back off** at the first of: an overflowed frame, a reply lost while
  more than one request was out, a busy answer, or replies past P2 on a
  module that answered within it at the step below. The cap drops to
  the last step that was clean and holds there for the rest of the
  session; a reconnect starts again from the default. Through the VX on
  J2534 a second module's single-frame reply is lost while the first
  module's segmented reply comes in (Known Issue #42, BENCH prep #35),
  so on the VX a pick with segmented replies backs the cap off at its
  first loss, whatever a bench run without them shows.
- **Fix it for a run:** `?inflight=<1 to the ceiling>` holds the cap at
  that value with the policy off, for measurement and tests; without it
  the policy runs.
- **What the buffers allow:** J2534-1 (2022) requires at least 4128
  bytes of receive and of transmit queue for each ISO 15765 logical
  channel (section 6.10, Tables 4 and 5), 45,408 bytes for a CAN
  channel and its logical channels together (section 6.5.6). It states
  nothing for v04.04's one channel, and the OBDX Pro developer's
  reference states no buffer size for the VX. So the overflow flag and
  the losses are the measure, not a figure from either document.

**More than one module asked at once:**

- **J2534:** each module has its own flow-control filter on the one
  ISO 15765 channel and its own outstanding request; the worker sends
  to every module with a free slot on each pass, up to the cap. Every
  request this ADR sends is a Single Frame, so v04.04's one write queue
  costs it nothing, and v05.00's logical channels stay where the
  roadmap has them.
- **SocketCAN:** the worker sends to the next module with a free slot
  as soon as the last request's echo is in, so several modules have a
  request out while replies overlap; the cap and the policy apply as on
  J2534. Its overflow is the kernel's (`SO_RXQ_OVFL`); a write refused
  for buffer space counts as a back-off too.
- **ELM327:** one command on the line, so the cap is 1 and the lever is
  the order. The engine gains a second order beside the next due
  request whichever module: a module's due requests asked together
  before the switch, at most one sweep of its picks before the next
  module's turn, so `AT SH` goes out once for each module's turn
  instead of once for each request. Which order a session uses is set
  by the measured switch: the faster on the adapters on hand is the
  default, with both kept and tested.

### 12. Tests

| Piece | Tier | Cases |
|---|---|---|
| The ABI | `test_c_abi.c` on both widths, `test_signals`, abidiff | The decl's 16 bytes and its reserved words refused when set; layout 4 refused; the info's size, its first four pairs and `module_count`; the getter past the count; every struct the reports use, unchanged; a set with a repeated name validates, and a format 1 or 2 file with one is still refused |
| Tables from the set | Unit (`RequestEngineTest`, `CoreTest`) | A set of several thousand requests with a pick of 20: the engine's table and the timing pool's sizes; a row taken at first ask and kept; a pick past the pool refused with nothing changed; an unranked set past the pool asked for nothing; no allocation on the worker (`WorkersAllocateNothingPerFrame`) |
| The ceilings | Unit (the signal-file and plugin suites), fuzz | A module past 1024 requests, a set past its signals or its copy's bytes, a 33rd module: each refused with its words; a variant of several data files past one file's 1024 signals loads, writes its copy and reads it back |
| The merge | Unit (a suite of its own in `test_core`), `fuzz_signal_file`'s corpus through the merge | Each rule (the same key, the same address, added), the order and the own variant last; a name alone never replaces, and two signals of one name are both kept under it; a base def never replaced, by a DID of its name, by a signal at its address or by a format 1 or 2 part that names it, which is added beside it with its note; a format 3 part merged alone equal to the part's set; each part's `source` and its share-alike flag read back, a replacing signal's part its own; each note's words; modules joined by their IDs, renamed, left out; a replaced key found again; an expression across parts, `needs` met and unmet and a chain left out; the parts, each def's part and the notes; deterministic bytes; the text read back to the same set; every refusal's words |
| Live modules | Unit (`RequestEngineTest`), Harness (`test_j2534_*`, `test_elm327`, `test_vcan`) | A module opened by a pick and closed when nothing picks it; a module nothing picks sent nothing; never more than the cap outstanding; the longest-waiting module takes the slot; a refused filter leaves one module closed and the rest open (a fake DLL knob); the ELM327 filter sent again on a change |
| The simulated car | Unit (`SimEcuTest`, `SimAskTest`, `SimDidsTest`), Harness on `vcan` (`SimBusTest`) | 32 nodes from a set; a functional request answered by each; a fault on one module alone and each module's own count; one session reading the made-up car whole, a segmented reply from two modules at once |
| Faster | Unit, Harness | Section 11's raise, back-off and hold at each signal; `?inflight=`; `AT SH` counted for each order on the fake ELM327; three fake DLL knobs (filters refused past a count, a module busy past a number of requests out, Known Issue #42's shape) |
| The key | Unit (`test_signals`, the signal-file suites) | The address key of each field shape, the same from two parts and two sets, never an author's key; a computed signal's and the later of two at one address under their own; a format 3 file refused two signals at one address |
| The recording | Unit (`RecorderTest`, the reader, export and health suites), `fuzz_recording` | Format 4's keys in `signals.csv`, the `signal`, `focus` and `pick` lines; two signals of one name recorded, read, queried and exported apart; a name one signal has and a key both taken, a repeated name refused with its keys; recordings in formats 1 to 3 read as before; a merged session's `plugin` lines, its copy past 1 MiB read by the reader, a replay and a scrubbed export; `module` lines; a `latency.csv` row for a fifth module |
| The front ends | `test_cli` goldens, `python/tests`, `BridgeTest`, `QmlTest.settings_*` and `.phone`, a `run-havne` smoke | `--together` and `plugins together`; `SignalFile.merge`; `--signal` by a name and by a key; a vehicle's picks merged and registered, the saved address moved, a layout's `key:` across a re-import; a tick kept by key across a change of picks, and one kept by name taken once; two rows of one name told apart by module, by part and by key; the page at 384 and 320 px |
| The bench (reads only, on request) | Run sheets | "To measure" below |

Each new test is mutation-checked.

### 13. Steps

Each a branch, core first; a step's public header change regenerates
the Python declarations in its commit, and each step runs the cross-arch
suite.

1. **The constants and the layout** (`feat!`): section 6's constants
   but `ECUIO_MODULE_MAX_REQUESTS`, `ECUIO_SIGNAL_SET_MAX_SIGNALS` and
   `ECUIO_SIGNAL_SET_MAX_BYTES`, which step 2 brings with their
   refusals; layout 5, the info's `module_count` and the getter, the
   reports and a module's state sized from the set, every caller's
   arrays moved to the count (the app, `havne-cli`, Python), the
   loaders' and the importer's words.
2. **Tables from the set:** the engine's table and the focus's marks
   from the set; timing rows taken at first ask; the ceilings for each
   module and each set and their refusals; a set's bounds for a merged
   list and its copy; the loader and the importer stop skipping signals
   at the cap.
3. **Keys before names:** a def's address key; recording format 4
   and what reads it (the reader, the exports, the health report,
   `--signal`, the app's and the viewer's ticks by key); then the
   validator takes a repeated name.
4. **The merged set:** `ecuio_signal_file_merge`, its readers, format
   3's `parts`, `needs` and an ECU's `name`, replaced keys; unit, fuzz
   and the ABI checks.
5. **The engine and the transports:** a module opened by the pick and
   closed when nothing picks it, on each transport; the slot for each
   module, the cap as a parameter at 4, the transport's ceiling and the
   counters; the recorder's `module` lines.
6. **The simulated car:** nodes to the ceiling, faults for each module,
   the made-up whole-car plugin, the `vcan` cases; the measurements
   that need no car.
7. **The bridge and the store:** a vehicle's picks read together (the
   merge on a change, the registration, the row's one button, the saved
   address), the Signals page, Live's module filter and what it shows
   beside a repeated name; `--together`,
   `plugins together` and Python.
8. **Faster:** the cap's raise and back-off with `?inflight=`, the fake
   DLL's knobs, ELM327's second order, its default set by the measured
   switch.

## Alternatives considered

- **Only raise the constants** (requests to 4096, channels to 32).
  Every session, the mock's too, would hold tables for the largest set,
  and a session would still ask everything it defines. It fixes the
  refusal and keeps the 40 s sweep.
- **Several sets side by side in one session.** Each module, or each
  plugin, its own profile, loaded together. It breaks the one ID space
  that computed signals across modules, gauges, the focus and a
  recording's `signals.json` rely on.
- **Ask everything, the unpicked slowly.** Every crawl request still
  takes a slot from the picks, the values it returns are minutes old,
  and on a set of thousands it never finishes a pass.
- **The default pick in core.** A front end would then override it to
  add gauges and ticks, and there would be two mechanisms for one pick.
- **A timing row for every defined request.** 1.5 KB for each request a
  set defines whether or not it is ever asked: about 6 MB for 4,000.
- **Timing rows allocated when a request is first asked.** The worker
  allocates nothing; a pool taken at create gives the same bound.
- **Rotating modules through a few slots.** It would help only an
  adapter that holds very few filters, at the cost of a filter stop and
  start and the extended session on every turn.
- **No cap on requests in flight.** Nothing has measured what a car's
  bus and an adapter's buffer carry at 18, and the VX already loses
  replies at two (Known Issue #42).
- **A fixed raised cap for each adapter.** It can't follow a car whose
  modules answer slowly or a pick heavy with segmented replies; the
  counters see both, and `?inflight=` gives a fixed value where one is
  wanted.
- **The info grown to hold 32 pairs.** One struct for a caller, but a
  caller built against 432 bytes would have core write past its
  buffer. The count and a getter break less.
- **The set's layout left at 4.** No rebuild for a hand-built set, but
  sessions as data would then need the next layout in the same release.
- **The merge in the app.** Less core work, but the CLI and Python
  would merge by their own rules or not at all, and the written copy's
  checks would be Qt's, not the loader's.
- **The address lists the parts** (`profile=<a>,<b>` merged by core at
  create). The address would say what decodes in words, and nothing
  would be registered; but a profile name holds 63 bytes and the info's
  field 64, so the list fits neither, a lookup by name (Add with no
  session, the simulated ECU's `profile=`) would have nothing to find,
  and the registry would need each profile's file as well as its set.
- **A name for each vehicle** (`vehicle.<n>`). Short, but it means
  something on one device only, and the CLI has no vehicles.
- **The profile named by its bytes.** Exact, but the name would change
  with every update of a part, and every saved address with it.
- **A plugin's keys in a space of their own** (`key:<plugin>/<key>`).
  Two plugins could then hold unrelated signals under one key; but a
  layout's `key:boost` reads boost from whichever file has it (ADR-0057
  section 2), which is the point of a key, and a second form of
  reference would reach every layout and condition.
- **One name, one signal** (a later plugin's signal replacing an
  earlier one of the same name). Simple to say, but a name is what an
  author chose to call a value, not where it is read: two modules'
  "Battery voltage" are two readings, and the later would hide the
  earlier.
- **A second name for a repeated name** ("Battery voltage (BCM)", or
  a number as the OBDb importer numbers a repeat inside one file), with
  names kept one each in a set. The name a signal is stored and
  recorded under would then depend on what else is picked: a tick or a
  recording's column made with two plugins wouldn't be found with one.
  A key from the address doesn't move, and the module is added where
  two names are shown together, which is the only place it is needed.
- **The base's one replacement by name kept in format 3.** A file
  could still say "read engine speed from this DID" and take the
  standard signal's place and ID. Every layout, condition and
  comparison that reads `id:7003` would then read whatever a plugin put
  there, on one vehicle and not on the next. A DID beside the PID costs
  a row and keeps the standard signal the same everywhere.
- **Share-alike over the whole merged copy.** Simple to state, but it
  would put OBDb's terms on a user's own signals and on a
  manufacturer's package for sitting in one file with an import, which
  the parts' separate marking is there to prevent.
- **The base's defs replaced by address too.** A part's signal at an
  `obd2` def's address would take its place. A file loaded alone keeps
  both, so a vehicle with one plugin would read another set through the
  merge than through the plugin's own profile.
- **`extends` naming a plugin's variant** (ADR-0064 Q8's option a). A
  load order between plugins, and a whole plugin refused when its base
  drops a key; reading together gives the reach with neither.

## Consequences

- A signal nobody picked has no value, no recording and no answer to
  "does this car have it" until it is asked once.
- A recording of a session is a recording of its pick. A replay shows
  what was asked, and "Not asked" for the rest has no meaning there.
- Every front end needs the fourth state, and a way to pick: the app's
  tick, `havne-cli`'s names.
- One ABI break: the channel ceiling, the set's layout and the info's
  module count. A set built by hand, and every caller of the reports,
  is rebuilt against it.
- On a large set, a health report and Diagnostics cover the open
  modules only; the whole vehicle's codes are discovery's.
- A session's rate can change during it, as the cap climbs or backs
  off; reply timing shows it, and a recording's header names
  `?inflight=` when the address holds it.
- Over a long session a request keeps its timing row once asked, so a
  user who ticks through more than 1024 different requests is refused
  the next until a reconnect.
- A vehicle reads one set whatever it came from. A signal a later
  plugin replaces, by its address or its key, is gone from the list
  (the notes name it), and two unrelated signals that share a key can't
  both be read; the notes say which won and that the addresses differ.
- Two signals that share a name are both read and both keep it. A
  list shows the module, the part or the key beside them; a name alone
  no longer says which signal is meant, so `--signal` and a query by
  name refuse a repeated one and say its keys.
- Recordings move to format 4, keyed by key. Older recordings read as
  they are; a tool outside Havne that reads `signals.csv` by its
  `signal` column finds keys there and the names in the header.
- `obd2`'s signals are the same in every vehicle's set. A file that
  replaced one by name keeps doing so only as a format 1 or 2 file read
  alone; in a vehicle's merged set both are read.
- An address names a merged profile by a hash, which says nothing by
  itself; the Log and the recording's `plugin` lines say what it is.
  ADR-0052 section 4's "the URI says exactly what decoded it" holds for
  one plugin and, for several, through those lines.
- A recording's `signals.json` holds every picked plugin's signals,
  each attributed to its part and under its part's license, so sharing
  a recording shares them all, the share-alike ones on their terms.
- A plugin's signals can be left out by another plugin's change (a
  dropped key, a module's IDs changed). It is a note, never a refused
  connect.
- Format 3 files (`parts`, `needs`, an ECU's `name`) are refused by an
  older core with words; a merged copy is always format 3.

## To measure

Each before the step that rests on it.

- The sizes read from the structs, with `sizeof`: a module's state
  (the engine's channel, the three reports, each transport's reassembly
  state), and a session's memory for a set of 4,000 signals over 18
  modules.
- How many flow-control filters the VX takes on one ISO 15765 channel
  through its J2534 driver, and the same for any other adapter used
  with J2534.
- How many requests in flight a car's bus and the VX's buffer carry
  without a loss, at each step from 1 up with `?inflight=`, with and
  without segmented replies (Known Issue #42), counted against a
  SocketCAN listener on the splitter.
- An ELM327's channel switch (`AT SH`) on a session of several modules,
  for each order: its time, and which order sweeps faster on the MX+
  and the VX in ELM mode.
- The engine's rotation and `ecuio_take_focus` over a table of several
  thousand requests with a pick of 20: the cost of stepping past parked
  rows, and of the maps' refresh when the table changes.
- A focus change's time on the worker, since a tick makes one.
- The loader's build over a list of several thousand signals (its name
  and key searches compare every pair), and the merge of a vehicle's
  parts: its time on the test phone, and whether it leaves the GUI
  thread.
- The bytes for each signal in a written copy, against the set's 4 MiB
  and 16,384 signals, on the made-up whole-car plugin.
- The signals in the OBDb sets on hand against a data file's 1024: a
  set past it needs the importer to write a data file for each module.
- The app's memory on the test phone with a set of 4,000 signals loaded
  and 20 asked, and Live's list with thousands of rows, most of them
  "Not asked".

## Decided

Each choice of sections 2, 6 and 9, 2026-11-15:

- **The merge's order:** a later pick replaces an earlier one, and the
  vehicle's own variant is always last. Not the first pick winning.
- **A signal's identity is its address** (the module, the request, the
  field), or its key. A name alone never replaces: two signals with one
  name and different addresses are both kept.
- **Names need not be unique.** Identity is the key core derives from
  the address; a name is a label and may repeat, and a screen showing
  two of one name adds the module beside each when it is drawn. No
  second name is stored, so nothing kept depends on what else is
  picked.
- **A recording writes keys,** with names as labels, in a new format;
  older recordings read as they are.
- **`obd2`'s standard signals are never replaced.** A DID reading the
  same quantity is a signal of its own beside the PID's. Format 1 and 2
  files keep ADR-0051's replacement by name; format 3 has none.
- **Share-alike stays with what came in share-alike.** A merged copy
  keeps each signal's source and license, and OBDb's terms don't reach
  the other parts.
- **The merged profile's name** is `merged.<16 hex>` from the parts'
  profile names. Not an address that lists the parts.
- **The profile row** has one button for the vehicle's set beside
  Default. Not a button for each pick.
- **`needs` and signal-file format 3** go with the merge, in v0.9.1
  (the cut of 2026-11-19).
- **An unranked set past the pool** is asked for nothing until a signal
  is picked. Not its first 1024 requests.

## Appendix: step 2 as built (2026-11-16)

The body stays as accepted. This says what step 2 built differently from
sections 2, 3, 6 and 8, each with its reason:

- **The cap is the rotation's, not a life's count (section 3).** The body
  has `ECUIO_PROFILE_MAX_REQUESTS` as "the most requests a session has in
  rotation over its life" with a pick past the pool refused. As built it
  is the most at one time: a pick (with the attached conditions' signals)
  is refused only when its own rotation would number more than 1024
  requests. A request first picked once all 1024 timing rows are taken is
  asked with no row, and counted in `ecuio_session_timing_t`'s `untimed`.
  Reason: a lifetime count would refuse the 1025th signal a user ever
  ticked in one session, however short each pick, which is no guard
  against a long rotation.
- **`ECUIO_SESSION_INFO_PICK_ONLY` (section 2's new case).** A set that
  derives more than 1024 requests makes a session that asks only its
  picks, whatever it ranks: every request starts parked, a focus of count
  0 parks them all, and the info flag says so from create (only with
  `ECUIO_SESSION_INFO_REQUESTS`). `ecuio_session_get_unasked_signals`
  lists every signal but an attached condition's from create.
- **A condition's signals on such a session (section 8).** They are
  always asked, focus or none, since core owns rule 4.
  `ecuio_session_set_conditions` is refused, with nothing changed, when
  its signals' requests with the focus's would number more than 1024.
- **`ecuio_signal_file_load_with` in place of a wider
  `ecuio_signal_file_load` (section 3's last bullet).** The body has
  `ecuio_signal_file_load` read to a set's bounds. As built the plain load
  keeps a data file's bounds and a reader of a set's written copy says so
  with `ECUIO_SIGNAL_FILE_LOAD_SET`. Reason: every import path and every
  front end's own file read calls the plain load, and widening it would
  have widened each of them without a word.
- **Link health on such a session.** While nothing is picked, a worker's
  idle pass counts no quiet read, so the session's own silence never
  reads as "not responding". What it did ask still counts:
  identification, diagnostics and the PID walk run with nothing picked,
  and a vehicle that answers none of them reads "not responding". Three
  rules follow from nothing being asked after that:
  - **One module's silence is its own.** While any module answers, a wait
    that runs out on another counts toward no timeout (the engine keeps a
    bit for each module: set by a frame from it, cleared when a wait on
    one of its table's requests runs out, a pick's or a check's). An
    identification or diagnostics item that goes unanswered leaves the
    bit: an ECU answers nothing to an item it doesn't serve, and that says
    nothing of the ECU. A whole-car profile on a trim that lacks a module
    reads OK, at connect and after a recovery asks the unanswered items
    again. With a rotation the other modules' replies between two
    timeouts say the same. What follows from it: once a module has
    answered, such a session reads "not responding" again only after a
    pick's request or a check times out; "Read again" on a vehicle
    switched off since leaves the link OK with its items unanswered.
  - **The check.** While the link isn't OK, the worker asks one request
    every 5 s (the failure backoff's cap), the first a whole interval
    after the link was found down: one request of the next module that
    isn't dropped, each module in turn, and for a module the request
    after its last that went unanswered (so one request an ECU is silent
    on isn't the only one ever tried, and one that never went out is
    asked again). A pick, or the link's recovery, before a check goes out
    withdraws it. Its reply is a frame like any (recorded, logged, timed,
    decoded) and brings the recovery every session has: the walk and the
    unanswered identification and diagnostics are asked again. Its
    silence changes nothing, so the state never flaps. With the link OK,
    or with anything picked, no check is asked. It is traffic on a session
    with nothing picked, as the walk and identification already are; it is
    the one way such a session sees an ECU wake after a connect with the
    ignition off.
  - **A hard failure ends on clean reads.** Once the adapter has read
    cleanly for a quiet run's passes (ten), a hard failure steps down: to
    OK when nothing went unanswered, else to "not responding", where the
    check settles it.

  Picks that were all dropped (refused by the ECU) still count quiet, as
  any table's do, and that "not responding" ends when the picks are taken
  away.
- **`havne-cli --signals`** holds a lone file to a data file's bounds, as
  the wrap does; only a recording's own `signals.json` reads past them.
- **The least core (ADR-0064 Q1, 2026-11-17).** Three things a set may
  hold are refused by a core before 0.9.0, and each raises a file's or a
  plugin's least core to 0.9.0 at the gate that reads it
  (`core/src/least_core.h`'s `kSetRequests`, `kSetSignals`, `kSetBytes`):
  more than 1024 requests in all, a variant's files' signals together past
  one file's 1024, and a variant's written copy past one file's 1 MiB.

## Appendix: step 3 as built (2026-11-17)

- **The address key's text (section 9 left it to the build).**
  `<id>:<key>[@<offset>]:<byte>.<bit>:<bits>` for a number or an enum,
  `<id>:<key>[@<offset>]:<byte>:t<bytes>` for a text or hex field:
  `message_id` in upper-case hex (3 digits up to 0x7FF, 8 above), the mux
  key's `mux_length` bytes in upper-case hex (empty with no mux) with
  `@<mux_offset>` only when that isn't 0, then `start_byte`, `bit_offset`
  and `bit_length` in decimal, or `start_byte` and `t<byte_length>`. The
  key bytes are the reply's as the def holds them, so a broadcast frame's
  signal has a key too (`1A0::0.4:12`). Byte order, kind, scale and offset
  are no part of it. It holds a `:` and upper-case hex, so it is never an
  author's key (`[a-z0-9_]`) nor `id:<n>`; it has no comma, quote, space
  or line break and starts with a hex digit, so it needs no CSV quoting
  and starts no formula; it is at most 31 bytes, the room a name and an
  author's key take.
- **A def with no room for its text (an addition to the approved calls,
  which named no failure but a NULL).** A def the validator refuses may
  make an address key's text past 31 bytes (fields past a frame's).
  `ecuio_signal_address_key` then answers `ECUIO_ERR_INVALID_ARGUMENT`
  with the key empty, since a refusal beats a cut key, and
  `ecuio_signal_set_keys` keeps such a def under its author's key or
  `id:<n>`. A def the validator takes always has room.
- **A set's keys are a filled array, not a handle.** The caller owns one
  allocation of 32-byte keys in set order, by the serial ports' contract,
  built once (a sort by address). The file is a parameter because the
  registry keeps only a file's set: with no file a computed signal goes by
  `id:<n>`. `ecuio_signal_set_keys` gives a computed signal and the later
  of two defs at one address the author's key from the file, else
  `id:<n>`.
- **One find rule in core.** `ecuio_signal_set_find` (and, with format 4,
  `ecuio_recording_find_signal`) is the rule for `--signal`, a query and a
  column a person names: a key first, then a name one signal has; a
  repeated name is refused with the first 8 of its keys and "and N more".
- **Format 4 is format 3 with keys.** The first line is `ecuio session
  recording, format 4 (ADR-0049)`. `signals.csv` is `host_us,<key>,<value>,
  <unit>`. The header's lines: `signal  <key>,<name>,<kind>,<unit>,<dp>,
  <rank>,<group>`, `label  <key>,<raw>,<label>`, and `focus` and `pick`
  lines (and their activity lines) of keys. A key needs no CSV quoting.
  Everything else is format 3's, so every reader gate on format 3 is "3 or
  later".
- **The recorder's keys** come from the signal file
  `ecuio_recorder_signal_file` gave it, made in that call while it has the
  file, else from the set alone, where a computed signal goes by `id:<n>`.
- **The reader** fills `ecuio_recording_signal_t.key` (four of the
  reserved words at offset 96) in a format 4 recording and in one opened
  through its frames; a row whose key no `signal` line declares shows its
  key as its name. Older formats have `key` "".
  `ecuio_recording_open_frames_with` takes the set's signal file so the
  keys are the ones a recording of that file is written under.
- **The exports.** The long CSV's fifth column is the key, and the wide
  CSV's head cell gains ` [key]` only where a head cell repeats (chosen
  over a key on every column: a spreadsheet header stays readable, and a
  second header row would break a reader that takes the first row as
  names). The folder export copies rows and header as they are; its scrub
  matches a text or identification def by key.
- **Left to steps 4 and 5:** the other format 4 lines of section 6
  (`plugin` lines for a merge, `module` lines, the health report's
  modules). A reader passes by a line kind it doesn't know.
- **The CSV import keeps writing format 3** (and format 2 for a log with
  no enum): its signals are names, and nothing has an address.
- **`havne-cli` names a signal by its key.** A line that names a signal
  by its name carries `key` after it (`null` before format 4); the
  lines' tables are in ADR-0031's note of 2026-11-17. `--signal` follows
  the find rule, and `view --frames` decodes with the file's keys.
- **The viewer by key.** The viewer keeps a signal's pane, and names the
  plotted export's columns, by the signal's key. An older recording keeps
  its panes by name in both its values and its frames opens (its values
  have names alone); its plotted export names a column by key in a frames
  open, by name in its values. A pane kept by name is read while the key
  has none, and in a format 4 recording the next change writes the key. A
  frames open is keyed by the set's signal file
  (`ecuio_recording_open_frames_with`): the recording's own
  `signals.json`, or a registered profile's file, whose text the app
  copies on the GUI thread for the open's thread to load its own copy,
  kept only when its set (the defs, channels, enum labels and computed
  programs) is the registered set's byte for byte, since the file's set is
  then what the frames decode by. A profile with no file opens with its
  set alone (`id:<n>` for a computed signal).
- **Compare's pairs.** One rule, `cli/signal_pairs.h`, for `havne-cli
  view`'s `delta` and the viewer's Compare (its dashed traces, the
  cursor's values there, the difference and the rise or fall alignment): a
  signal pairs with the other recording's signal of its key, when it has a
  key the other holds; else with the signal of its name, when that name is
  one signal's in each recording and that signal isn't the pair of a key
  this recording holds. A renamed signal keeps its pair by key, and a new
  signal of its old name takes none. A name two signals of either
  recording have pairs nothing by name, since nothing says which of them
  the other recorded. An `id:<n>` key is a set's own numbering, so it
  pairs by name. Keys on both sides with none in common still pair by a
  name each has once, so an `obd2` recording still compares with a
  manufacturer plugin's. The unit is the caller's: the dashed trace takes
  any pair; `delta` and the difference take a number in the same unit, its
  case aside. `view --align-on rise:<signal>:<level>` finds the signal in
  each recording by core's find rule (a key, else a name one signal has; a
  repeated name is usage); the app aligns on the signal picked here and
  its pair there.
- **The validator takes a repeated name (section 6).**
  `ECUIO_VALIDATE_ERR_DUPLICATE_NAME` stays defined, retired, never
  returned or reused, so no caller's build breaks on it. The loader keeps
  a format 1 or 2 file's refusal of two signals of one name ("a name names
  one signal"), and signal-file format 3 (step 4) is where a file first
  holds one; until then a repeated name reaches a set only from a set built
  in code and registered through the C API. Python's `Session` finds a
  signal by core's find rule (`ecuio_signal_set_find`: a key, else a name
  one signal has; a repeated name is a `KeyError` with core's words, which
  list the keys) for `set_focus`, `ranked_signals` and a health report's
  values. A health report read from a recording's values by name (before
  format 4, no frames) gives no value to a name two of the set's defs
  share, since nothing says whose rows they are.

## Appendix: the hold for Known Issue #42 (2026-11-18)

Section 11's back-off answers a lost reply by slowing down. Through the
VX, two or more modules lose replies while a segmented reply is on the
bus, and slowing down doesn't remove the loss, so the engine holds: with
two or more channels, no other channel is asked while one channel's
segmented request is on the bus (`core/src/j2534_hold.h`). Section 11's
back-off clause stays as the fallback for a loss the hold doesn't cover.

- **The window and its evidence:** a Single Frame reply that ends while
  another module's segmented reply is on the bus, from its First Frame to
  its last Consecutive Frame, is lost
  ([2026-11-11-monitor-car](../bench-sessions/2026-11-11-monitor-car.md),
  R1 and R2; D11 of
  [2026-11-18-pacing-car](../bench-sessions/2026-11-18-pacing-car.md): the
  window is 47% of the time and 45% of the second module's replies were
  lost).
- **The rules:** a request expected segmented (`ecuio_engine_reply_segmented`)
  is kept chosen until no other channel has one on the bus; a request in a
  pending wait, and a silent module's, hold no one; a session request goes
  through only near S3, and a key goes first. `?hold=0` switches it off.
- **Not yet measured on the car:** the hold's prediction (the second
  module loses next to nothing, the first keeps about 0.64 of its rate) is
  the pacing model's. Known Issue #42 stays open until a car run confirms
  it.

### The order is per signal (2026-11-20)

The channels that wait for the bus are ranked by the age of each channel's
next fast row (the time since that row was last written), not by turns,
so a module with more rows takes more turns and each signal is asked
about as often as every other. Channels whose last write was a Single
Frame go first; a Single Frame channel whose reply outlasts its channel's
floor takes one extra turn for each segmented exchange, so a big Single
Frame module keeps up with the segmented ones. A row waits at most for the
Single Frames in flight, one extra pass and each older row once. Whether a
row's first ask shares the bus is judged by its defs and, for a Service 01
PID, by J1979's length, until a reply shows its frames. On the pacing
model's four-module shape through the VX, equal turns gave 102.8 values a
second with the slowest signal's median at 1,573 ms; the order gives 137.4
and 721 ms, and the first laps lose no reply. The figures are the model's;
no car run has measured them.

## Appendix: notes from the review pass (2026-11-20)

- **Section 9, "Keys stay stable":** of two defs at one address the earlier in the set's order keeps the address key, whichever was written first; the other goes by its author's key (or `id:<n>`). A signal placed ahead of another at its address takes the key, and a vehicle's ticks and later recordings follow it. `signals.h` states the rule; `SignalKeysTest.TheEarlierOfTwoAtOneAddressKeepsTheAddressKey` pins it both ways.
- **Step 2's appendix, "The check":** a check's reply is timed only at a row a focus once kept. The check takes no row of the timing pool, since its turn goes around a table past the pool's size and would spend the pool on rows nobody picked.
- **The hold, absent modules:** under the hold an absent module holds the bus only for its first ask's wait, 100 ms once if that ask reads long: four present modules beside 14 absent ones answer 92% of their own count in the first 3 s when the absent ones' rows are short, 40% when they read long (the hold's test bus, `J2534HoldTest.AbsentModulesHoldNoOne`; 40% and 1% before). A module asleep at connect is judged by its defs again from its first frame after the silence; that waking reply can cost each other module one Single Frame, once (3 lost against 24 before, three present modules beside one of 20 long rows waking at 5 s).
