# ADR-0027: Vehicle profiles — a named vehicle remembers its connection, its plotted signals and its recordings' name, and the VIN recognizes it

- **Status:** Accepted.
- **Date:** 2026-10-27. Implemented with this record.
- **Builds on:** [ADR-0021](0021-vehicle-identification.md) (the VIN,
  asked once per channel at connect, whose context names per-vehicle
  profiles as the key's purpose); [ADR-0017](0017-session-recording.md)
  (one recording folder per connect, named by date); [ADR-0013](0013-protocol-profiles.md)
  and [ADR-0020](0020-uds-generic.md) (what a URI's query selects).

## Context

Each time the app meets a car, the user sets it up again: the adapter,
the profile, `uds-generic`'s DIDs, the signals plotted on Live. The
app keeps one last URI that connected and the Record switch
(SettingsPage's QML `Settings`), which is right for one car and wrong
for two, and recordings are named by date alone, so a folder of them
can't be told apart by car without opening each `events.log`.

Two facts shape a fix:

- **The VIN arrives after the connect.** Identification runs on the
  open session (ADR-0021 §1), a second or two in, while the profile and
  the DIDs are in the URI the session was opened with. A vehicle's
  settings that shape the session therefore have to be chosen before
  connecting, and the VIN can only confirm or correct that choice.
- **The VIN is personal data.** The project scrubs it from every
  committed session record (`docs/bench-sessions/`); the preferences
  are stored in the registry (Windows) or an ini, which a user may
  paste into a bug report.

## Decision

### 1. A vehicle is a named entry the user picks

A vehicle has a label ("Daily driver"), and remembers:

- **its connection:** the last adapter URI that connected while it was
  picked (`j2534://…` or `elm327://…`, the query whole: the profile,
  the DIDs and every other key);
- **its plotted signals:** the names of the rows unticked on Live
  (every numeric row starts ticked, so storing the exceptions keeps a
  signal new to the set plotted);
- **its VIN key** (§3), once a connect has learned it.

Settings gets a Vehicle section above the presets: one button per
saved vehicle, the picked one highlighted, wrapping onto more lines as
the list grows (a `Flow`, so it fits a phone), and "None", which picks
nothing so the app behaves as it does today. Picking a vehicle fills
the URI field with its connection, as a device row does.

**The label.** The section has one Label field. A vehicle is never
left unnamed, and typing is never required to make one: with the field
empty, New and Duplicate use a default, which a line under the field
names, and whatever the user types instead replaces it.

- New's default is "Vehicle N", the lowest N no vehicle's label holds.
  A vehicle §4 saves for an unknown VIN gets the same default, so both
  paths name alike.
- Duplicate's default is "<label> (copy)", then "<label> (copy 2)" and
  on; a copy's copy continues the count ("<label> (copy 2)", not
  "<label> (copy) (copy)").
- Rename takes only what's typed.

A label is any text of 1 to 40 characters after surrounding spaces are
trimmed, unlike every other vehicle's (ignoring case); no convention is
imposed past that. A button whose typed label breaks the rule is
disabled, and the field says why. No default is made from the VIN: its
manufacturer and model-year codes would need a table, and a make and
year in a label points at the car more than a number does.

Every change to the list is made there, on every platform, with no
file to edit. Each acts on the picked vehicle and uses no popup (they
don't fit the zoomed UI):

- **New:** an empty vehicle, picked.
- **Duplicate:** a copy of the picked vehicle's connection and plotted
  signals, picked, without its VIN key, so the next car connected
  under it is learned as itself (§4). One car's setup becomes the
  start of another's.
- **Rename:** the picked vehicle takes the typed label.
- **Forget the VIN:** clears the key, so the next connect learns
  again; for a car whose ECU was replaced, or one learned by mistake.
- **Forget:** removes the vehicle.
- **Forget all vehicles:** removes every vehicle and the salt (§3), as
  before the first one was saved.

The two that remove something ask once, in place: the
button's row turns into "Forget Daily driver?" with Forget and Cancel,
and Cancel or picking another vehicle backs out. Nothing in the
section is enabled while a session is connecting, connected or
disconnecting (the page's `idle`), so the vehicle a session runs under
can't change or vanish beneath it; §4's switch is the one change made
during a session.

A vehicle applies to adapter sessions only. A `mock://` or `replay://`
connect neither changes the picked vehicle's connection nor names its
recording after it.

### 2. What it changes, and when

- **Connect:** a successful adapter connect stores the URI as the
  picked vehicle's connection (beside the global last URI, which stays).
- **Live:** once the session's rows exist (`onConnectResult`), the
  rows the picked vehicle has unticked are unticked; a later tick or
  untick updates the vehicle's list.
- **Recording:** the folder is `<yyyyMMdd-HHmmss> <label>` (the label
  reduced to letters, digits, space, `-`, `_` and `.`, at most 40
  characters; `-n` still breaks a tie). The bridge takes the label
  before the connect (`Bridge.vehicleLabel`), because the folder opens
  before the first frame (ADR-0017) and long before the VIN.
- **The activity log** names the vehicle once the VIN settles it (§4),
  so `events.log` records which car a recording is.

### 3. The VIN is kept as a salted hash

The bridge reads the VIN from the first channel's identity once it is
complete: `09 02`'s text, or `22 F190`'s when `09 02` wasn't answered,
trimmed of padding and accepted at 17 characters. It never hands the
VIN to QML. It signals `vehicleIdentified()`, and
`Bridge.vinKey(salt)` returns the hex SHA-256 of salt and VIN. The
salt is 16 random bytes made once per install (`Bridge.newSalt()`,
`QRandomGenerator::system()`) and stored with the vehicles.

The settings then hold no VIN to read. This is obscuring, not
protection: a 17-character VIN of a known make and model has a small
enough serial space that the salt beside it doesn't stop a brute force.
The VIN itself stays where ADR-0021 put it, in the activity log and
`events.log`.

### 4. The VIN settles which vehicle it is

When the key arrives:

| Picked vehicle | Key | Result | Log line |
|---|---|---|---|
| one, with no key yet | any | the vehicle learns it | `Vehicle: Daily driver (recognized from now on)` |
| one, with this key | this | nothing changes | `Vehicle: Daily driver` |
| any or none | another saved vehicle's | that vehicle becomes the picked one | `Vehicle: Weekend car (picked; Daily driver was)` |
| one, with a different key | unknown | a new vehicle, "Vehicle N", takes the key and this connection and becomes the picked one | `Vehicle: not Daily driver (its VIN differs); saved as Vehicle 2` |
| none | unknown | the same, a new "Vehicle N" | `Vehicle: saved as Vehicle 2` |

A switch applies the new vehicle's plotted signals at once (they
don't touch the session) and its connection from the next connect;
the running session is never reconnected. The recording keeps the
folder it opened with, which may name the vehicle first picked; the
log line says which car it is. One key belongs to one vehicle.

### 5. Where it lives

The vehicles are one JSON document in QML `Settings` (category
`vehicles`, beside `session`), as `adapterVersions` is:
`{ "salt": "…", "picked": "<id>", "list": [ { "id", "label",
"vinKey", "uri", "unplotted": [ … ] } ] }`. The rules (parse and
validate, a label's validity, new, duplicate, rename, forget the VIN,
forget, forget all, recognize, remember a connection or a plotted
list) are pure functions in a `pragma Singleton` QML object,
`VehicleBook.qml`, that take and return the document, so
`tst_vehiclebook.qml` runs them without a page, as `tst_chartlayout.qml`
runs `ChartLayout`. SettingsPage holds the store, the row and the
`Bridge` wiring. A document that doesn't parse, or an entry missing a
field, is dropped whole, and the app starts with no vehicles (the
damaged text isn't overwritten until the next change).

The bridge's part is small and QtCore-only: `vehicleLabel` (a
property the recorder reads), `vehicleIdentified()`, `vinKey(salt)`
and `newSalt()`, the VIN cleared at disconnect.

## Alternatives considered

- **Key by the VIN alone, no picking.** The app would recognize the
  car and apply everything itself. But the profile and the DIDs are in
  the URI before the VIN exists, so the first connect to each car
  would always run on the last car's settings, and a reconnect would
  follow every recognition.
- **Reconnect when the VIN names another vehicle.** Applying its
  connection at once would be right more often, but it drops a
  session the user started and a recording it was writing, for a
  difference the user may not care about. Picking it for the next
  connect, and saying so, leaves the choice with the user.
- **The VIN in plain text.** Simpler, and a vehicle editor could show
  it, but the preferences would then carry it into any shared registry
  export or ini. Nothing needs to read it back: equality is all the
  match asks.
- **A C++ store in the bridge.** `QSettings` and a model class would
  put the rules under gtest, but every other preference lives in QML
  `Settings`, and two owners of one store would have to agree on its
  writes. Pure functions in a QML singleton keep one owner and still
  test without a page.
- **A default label made from the car, "<Make> <Model> N".** OBD
  carries no model: the VIN's manufacturer code (its first three
  characters) and model-year character are standard, but its model is
  in the manufacturer's own descriptor section, which only a
  maker-by-maker table or an online decoder reads. An online decoder
  sends the VIN off the device, against §3; an offline table of
  manufacturer codes gives the make alone, drifts, and misnames
  rebadged and imported cars. And the label reaches folder names and
  logs that get shared, where "Vehicle 2" says nothing about the car.
  A user who wants the make and model types them once.
- **Leave deleting and copying to the settings file.** The store is
  one JSON value, so a skilled user could edit it, but on Windows it's
  in the registry, on a phone it can't be reached at all, and a
  mistyped edit drops the whole document (§5). Each operation is a
  button instead.
- **Rename the recording's folder when the VIN settles it.** On
  Windows a folder with open files can't be renamed, and the folder's
  name is in the activity log already. The log line names the car
  instead.

## Consequences

- One car's setup comes back by picking its name, and a second car no
  longer overwrites the first's. A recording's folder names the car it
  was picked for.
- Every vehicle can be made, copied, renamed, unlearned and removed in
  the app, the same way on a desktop and a phone; nothing asks the
  user to find or edit a file.
- The VIN key is meaningful only with its salt, in this install. A
  reinstall that loses the preferences relearns each car on its next
  connect.
- A label is user text, and it reaches recording folder names, the
  activity log and `events.log`. A committed session sheet that cites a
  recording folder shows it, so the label is checked with the rest of
  the sheet's scrub before it's committed.
- A car that never answers `09 02` or `F190` is never recognized; its
  vehicle works by picking alone.
- A wrong pick before a connect costs one session on the wrong
  settings and a folder with the wrong name, and the log line says so.
- Dashboard arrangements (STATUS, "User-configurable dashboards") would
  join a vehicle's entry when they exist; the document has room.
