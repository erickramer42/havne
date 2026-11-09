# ADR-0058: Night mode as its own control, with an amber and a red style

- **Status:** Accepted; built in v0.7.1.
- **Date:** 2026-11-08.
- **Builds on:** ADR-0057 section 9 (the gauge roles the night palettes
  set), the phone's density rules (targets of 48 dp), and the contrast
  floors `tests/qml/tst_theme.qml` asserts.
- **Read against:** `origin/dev` bbc9050: `app/qml/Theme.qml`,
  `SettingsDisplay.qml`, `DashboardPage.qml` (the header, lines
  1212-1300), `Main.qml` (the top bar), `app/windowtheme.*`.
- **Out of scope here:** the button hierarchy, the tab bar, the type
  scale, Home, and the `/design` mockups.

## Context

v0.7.0 ships three themes behind one setting, `ui/theme`, with the
values `dark`, `lowGlare` and `night`, chosen by three buttons on
Settings' Display page and nowhere else. Every color in the app reads a
`Theme` role, the gauges included (ADR-0057 section 9), so a switch
repaints everything; the platform window follows (`WindowTheme`:
Windows' title bar, Android's window background).

The phone check of 2026-11-08 (v0.7.0's dev build on the test phone, the
Dashboard in each theme) and the review item it raised found three
things.

1. **Two of the three are the same idea.** Low glare and Night share
   their structure: near-black surfaces, warm text, danger told by the
   mark and the heavier stroke because red is everywhere. They differ in
   hue (amber against red) and in brightness. Both are for after dark.
2. **"Low glare" doesn't say when to use it.** A driver at night sees
   "Dark", "Low glare" and "Night" and has to try them. The name describes
   a property, not an occasion.
3. **Traces brighter than the text.** In both night palettes the first
   trace color is the brightest thing on the page. Measured (WCAG 2.x
   relative luminance, the formula `tst_theme.qml` uses):

   | palette | text | first trace | brighter traces |
   |---|---|---|---|
   | Low glare | `#f3c895`, L 0.625 | `#fff5bf`, L 0.903 | rows 0, 2, 7 (`#fff5bf`, `#ffd900`, `#ffe373`) |
   | Night | `#ff9484`, L 0.441 | `#ffdfbf`, L 0.778 | rows 0, 6, 9 (`#ffdfbf`, `#ffab73`, `#ebb0b0`) |

   The gauge readouts are brighter than the text too, by about one part
   in a hundred (`#f7c98a` L 0.634 against 0.625; `#ff9686` L 0.448
   against 0.441). Dark's near-white trace (`#f2f2f2`, L 0.888) is
   brighter than Dark's text (L 0.812) as well; Dark is the day theme,
   where glare isn't the problem.

   Why it matters: at night the eye adapts to the brightest thing in
   view. A trace at L 0.90 on a page of L 0.63 text sets the adaptation,
   and the text and the gauges read dimmer for it; the first trace is
   also the one every chart has.

The direction agreed on 2026-11-08: a look (Dark), a separate Night
mode (Off, On, On a schedule), a night style (amber or red), and a
one-tap switch reachable from the Dashboard. A fixed time schedule needs
no permission. Sunset needs location and the ambient light sensor is
phone-only, so both wait.

## Decision

### 1. Three controls in place of one theme

| Control | Values | Default | Stored key (`ui`) |
|---|---|---|---|
| Look | `dark` (the only one today) | `dark` | `look` |
| Night | `off`, `on`, `schedule` | `off` | `night` |
| Night style | `amber`, `red` | `amber` | `nightStyle` |
| Schedule | from and until, `HH:MM` local | `19:00`, `07:00` | `nightFrom`, `nightUntil` |
| Override | `""`, `on`, `off`, with its expiry | `""` | `nightOverride`, `nightOverrideUntil` |

The palette on screen is resolved, never stored:

```
nightActive = night == "on"
            : night == "schedule" ? (override != "" ? override == "on"
                                                    : inWindow(now, from, until))
            : false
active      = nightActive ? (nightStyle == "red" ? "nightRed" : "nightAmber") : look
```

The override exists only in schedule mode, and only until the next
boundary. Any change to Night, From or Until clears it, so a setting
made in Settings always shows at once: "Off" tapped on the Dashboard and
then "On" chosen in Settings is night on, not off until the boundary.
A change of the style leaves it (the style doesn't say whether night is
on).

`Theme.active` keeps its name and its job: the palette key every role
reads through. The palettes are `dark`, `nightAmber` and `nightRed`;
`Theme.themeNames` becomes `Theme.paletteNames` (the list the tests
iterate). The writable `Theme.theme` goes; the writable properties are
`look`, `night`, `nightStyle`, `nightFrom`, `nightUntil`, and
`nightActive` is read-only. A value outside the lists reads as the
default, as `clampTheme` does today.

Look is stored from day one so a second look (Light, following the
system's scheme) needs no migration, but it shows no control until a
second value exists: a row with one button teaches nothing (decision 3).

### 2. What each is for, and the names a user sees

- **Dark** (the look): day and indoors. The default, unchanged.
- **Night, Amber:** after dark in the cabin, the usual case. It is the
  more legible of the two by the measured floors (text on bg 12.7:1
  against red's 9.5:1), and its warning orange and danger red still read
  as different colors against the amber text.
- **Night, Red:** the dimmest palette, for the driver who must keep
  their eyes on an unlit road or a night stage and wants the least light
  from the screen; the warm-hue rule in `tst_theme.qml` (no green or
  blue in the gauge roles, to keep a driver's night vision) is what
  both styles are built on, and red takes it furthest. It costs the
  most: every role is a red, danger is told by the mark and the stroke
  alone, and the trace palette has the least room.

Strings: "Night" for the mode (its buttons "Off", "On", "Scheduled"),
"Night style" with "Amber" and "Red", "From" and "Until" for the
schedule. "Low glare" goes. The Dashboard's switch says "Night".

### 3. The palettes: surfaces and roles stay, the traces change

The two night palettes keep every surface and role they have today;
they met the floors (text 4.5:1, outline 3:1, the readouts' 5.3:1 at
the least) and the phone check read them well. Two rules are added and
asserted:

- **No trace brighter than the text** in a night palette: each series
  color's relative luminance is at or under `text`'s. Dark is exempt
  (decision 7).
- **The readout is never brighter than the text:** `gaugeReadout` in the
  night palettes becomes the text color (`#f3c895` amber, `#ff9484` red).
  The difference today is invisible; the rule then reads cleanly as
  "nothing on the page outshines the text" (decision 8).

The replacement traces keep every color that already met the rule and
replace the three offenders in each palette. Every number below is
computed, not judged (the scripts are in the appendix). Floors: 3.5:1 on
`bg` and on `surface`, 38 apart pairwise in RGB, hue 0-60 degrees,
luminance at or under the text's.

**Night, Amber** (text L 0.625; rows 0, 2 and 7 replaced):

| row | color | L | hue | on bg | on surface |
|---|---|---|---|---|---|
| 0 | `#f0b45a` (was `#fff5bf`) | 0.519 | 36 | 10.70 | 10.07 |
| 1 | `#ff4000` | 0.249 | 15 | 5.63 | 5.29 |
| 2 | `#e6c200` (was `#ffd900`) | 0.554 | 51 | 11.36 | 10.69 |
| 3 | `#9e5d47` | 0.156 | 15 | 3.86 | 3.64 |
| 4 | `#d17d00` | 0.282 | 36 | 6.25 | 5.88 |
| 5 | `#9e9147` | 0.280 | 51 | 6.20 | 5.83 |
| 6 | `#ffb299` | 0.554 | 15 | 11.36 | 10.68 |
| 7 | `#d9c27a` (was `#ffe373`) | 0.547 | 46 | 11.23 | 10.57 |
| 8 | `#ff794d` | 0.355 | 15 | 7.61 | 7.16 |
| 9 | `#b89c00` | 0.340 | 51 | 7.33 | 6.89 |

Least pairwise distance 39.8 (was 39.8); the nearest pair is unchanged
from today's palette.

**Night, Red** (text L 0.441; rows 0, 6 and 9 replaced):

| row | color | L | hue | on bg | on surface |
|---|---|---|---|---|---|
| 0 | `#ee9a7c` (was `#ffdfbf`) | 0.427 | 16 | 9.28 | 8.92 |
| 1 | `#ff0000` | 0.213 | 0 | 5.10 | 4.91 |
| 2 | `#9e5b18` | 0.148 | 30 | 3.85 | 3.70 |
| 3 | `#ff7373` | 0.348 | 0 | 7.72 | 7.43 |
| 4 | `#9e7777` | 0.218 | 0 | 5.21 | 5.01 |
| 5 | `#ff8000` | 0.367 | 30 | 8.10 | 7.79 |
| 6 | `#e08a4a` (was `#ffab73`) | 0.345 | 26 | 7.68 | 7.38 |
| 7 | `#d1431f` | 0.177 | 12 | 4.40 | 4.23 |
| 8 | `#b86253` | 0.196 | 9 | 4.77 | 4.59 |
| 9 | `#d9a3a3` (was `#ebb0b0`) | 0.436 | 0 | 9.44 | 9.08 |

Least pairwise distance 43.5 (was 49.1).

A search over the whole warm range (appendix) finds sets with a least
distance above 65 under the same floors, so the constraints leave room;
those sets reach it with olive and gray-brown members that read cold or
muddy on the phone, which is why the hand-kept colors stay and only the
offenders move. The first row in each is deliberately the most legible
trace (closest to the text in luminance without passing it), since it is
the one every single-signal chart draws.

Unchanged and re-stated so the numbers are in one place: text on bg
12.7:1 (amber) and 9.5:1 (red); every other pair's floor is asserted
in `tst_theme.qml`.

### 4. Stored settings and the migration from `ui/theme`

Theme reads its keys when the singleton is made, as today, so the first
frame has the right palette. When `night` is absent and `theme` is
present, the old value maps once:

| `ui/theme` | `night` | `nightStyle` |
|---|---|---|
| `dark` | `off` | `amber` |
| `lowGlare` | `on` | `amber` |
| `night` | `on` | `red` |
| anything else, or absent | `off` | `amber` |

`look` is written as `dark`. The old key is left in place and never
read again (a downgrade to v0.7.0 reads it as before; v0.7.1 never
writes it). The mapping is a pure function, `Theme.migrateTheme(name)`,
so a test covers every row without a second settings file.

The override and its expiry are stored too, so a tap at 18:00 (the
switch then says "On until 07:00", section 5) survives a restart on the
way to the car (decision 10). On load an expired override clears.

### 5. The schedule: what it does across a session and at the boundary

- **The window** is on the wall clock: `from` inclusive, `until`
  exclusive, and it may cross midnight (19:00 to 07:00 means on when the
  time is 19:00 or later, or before 07:00). `from == until` is refused by
  the Display page with the words "From and Until are the same time", as
  the dashboard's range refuses `lo == hi`; the stored pair never holds
  it. A clock change (DST) needs nothing: the comparison is of local
  times.
- **Evaluation.** `Theme.refreshNight()` recomputes `nightActive` from
  the clock, the settings and the override. It runs on every change of
  those, from a repeating 30 s `Timer` inside Theme, and when the
  application becomes active again (`Qt.application.state`), since a
  suspended phone's timers don't fire and the user opening the app at
  22:00 must see night at once. So a boundary takes effect within 30 s,
  on every platform, with no arithmetic about the next boundary that a
  sleep or a clock jump could break. Android's background sessions
  (ADR-0029) keep the GUI thread, so the timer runs on there too.
- **The clock is a seam:** `property var now: () => new Date()`, which
  a test replaces; nothing else in the app tells the time this way.
- **Across a session nothing but paint changes.** The bridge, the
  session, a recording or a running log see no event: the switch is a
  change of `Theme.active`, and every role is a binding on it. The
  strip chart repaints on the change (it does today, `QmlTest.liveview`
  asserts it), the gauges and cards follow their bindings, edit mode and
  the page's selection keep their state, and `WindowTheme` re-colors the
  title bar and Android's window as it does on a manual switch.
- **The boundary while the user is on another page** is the same event;
  there is no page-specific state.
- **A tap on the switch in schedule mode** with no override in force
  sets the override to the opposite of what the schedule says now, with
  its expiry at the next boundary after now (whichever of `from` and
  `until` comes first). A tap while an override is in force clears it,
  and the schedule rules again at once: so the switch always flips what
  is shown, and two taps return to the schedule. A change to Night, From
  or Until clears it too (section 1). A tap in On or Off mode flips the
  mode itself (there is no boundary to wait for), and Settings shows it.
- **The expiry and the words are two different times.** The override
  expires at the next boundary; at that boundary the schedule takes the
  override's own state (a tap always sets the opposite of the current
  window, and the next boundary flips the window), so the override ends
  unseen and the shown state next changes at the boundary after it.
  The words, on the switch and in Settings' summary, say the next time
  the shown state really changes: "On until `until`" when night is on,
  "Off until `from`" when it is off. The two cases:

  | tap at | schedule says | override, expires | shown | words |
  |---|---|---|---|---|
  | 18:00 | off (from 19:00) | on, 19:00 | on | "On until 07:00" |
  | 20:00 | on (until 07:00) | off, 07:00 | off | "Off until 19:00" |

  At 19:00 in the first row the override expires and the window opens:
  nothing visible changes until 07:00. `Accessible.name` carries the
  same words plus the action ("Night is on until 07:00. Turn it off").
  With no override, Settings' summary says "Scheduled 19:00 to 07:00".
- **The modes are append-only strings:** `sunset` and `sensor` join the
  list later without touching the stored shape.

### 6. The one-tap switch

**One component,** `NightSwitch.qml`: a `ThemedButton` in the compact
form, `objectName: "nightSwitch"`, its hit area `Theme.touchTarget` each
way on the phone, drawn as a pill like the top bar's link pill (28 to
32 px tall) with a small crescent drawn by a `Shape` (two circles, no
font glyph: the platforms' fallback fonts don't all have one) and the
word "Night". Off: the outline color and `textDim`; on: filled with
`primary` like a highlighted button. `Accessible.name` says the state,
the next change and what a tap does, the words of section 5 ("Night is
on until 07:00. Turn it off").

**Where it goes: at the end of the link state's row, on every form.**
The link state is the Dashboard's status line, and the switch is a
status with a tap. That gives one rule and three placements that fall
out of the page's existing layout:

- **Desktop** (`form === "desktop"`): the title row is `Dashboard`, the
  link state (fills), the switch, `Reset peaks`, `Edit`. Width is not
  the constraint there; the switch sits between the status it belongs
  with and the actions.
- **Phone on its side** (`Theme.shortLayout`): the link state is in the
  title row already (the `linkInRow` loader) and the switch joins it
  there, at 48 on the glass; the row is wider than 600 px.
- **Phone upright:** the link state has a line of its own under the
  title (the `linkLine` loader), and the switch sits at its right end.
  The line grows from the text's height to the switch's 48 dp hit area
  (the pill draws 32 inside it; the hit area can't overhang upward, since
  `Edit`'s hit area is right above). Cost: about 28 dp of the upright
  Dashboard, 3 % of the test phone's height, taken from the gauges.

Why not the title row on the phone upright: it holds the title and two
compact buttons, and at the default font it has about 96 dp to spare
(364 usable at 384 wide: title 96, `Reset peaks` 92, `Edit` 50, three
gaps of 10), so a 64 dp switch fits at 100 %. At Android's larger font
scales it doesn't, and the title gives way first (`titleShrinks`), so the
page would read "Dashb..." for the users who most need large text. The
rule of at most three buttons in a phone's row holds either way; the
font doesn't. When the button hierarchy (a v0.7.1 item) makes `Reset
peaks` a quiet control, the switch can move up and the line can shrink
back; the component doesn't change, only its slot.

**Density, checked:** the switch is a tappable control, so the phone
test's sweep of every target at 384 x 832 includes it with no new code
(`QmlTest.phone`'s target test); the upright row's one neighbor above is
`Edit` at `root.gap / 2` away, within the 8 dp rule once the line is 48
tall.

**No keyboard shortcut** for now (decision 11): the Dashboard's keys
page through the book; Settings has the control in two taps on the
desktop as well.

### 7. Settings' Display page

Rows, top to bottom, each a `LabeledFlow` as today, three buttons at
most in a row:

1. Display size (as is).
2. **Night:** `Off` `On` `Scheduled`. Takes effect at once.
3. **Night style:** `Amber` `Red`. Enabled always (a user may pick the
   style before the evening); the Dashboard's switch shows the result.
4. **From** and **Until** (shown when Scheduled): two `TextField`s with
   `inputMask "99:99"` and a 24-hour validator, stored on
   `editingFinished` when valid; the problem words below them in
   `Theme.danger` otherwise ("Not a time", "From and Until are the same
   time"). No popup time picker: the controls' popups don't fit the zoom
   (ADR-0057's note) and the native pickers differ per platform; a
   masked field takes the phone's numeric keyboard and the desktop's.
5. Units (as is).

The Display row's summary in the Settings list gains the night state
("Night off", "Night on, red", "Night scheduled 19:00 to 07:00"). On the
test phone the page stays under one screen.

### 8. The platform window

Nothing new: `WindowTheme` watches `bg`, `surface` and `text`, which move
when `active` moves, whether the trigger is a tap or the timer. The
Windows title bar changes color at the boundary with the page; Android's
window background likewise. (Android's status and navigation bar colors
stay as they are, the open item from the themes handover.)

### 9. Tests

`QmlTest.theme` (`tst_theme.qml`):
- the resolution table: mode x style x in-window x override, each row's
  `active`, including the cleared cases: an override cleared by a
  second tap, by a change of Night, of From and of Until, and by its
  expiry, each followed by the schedule's own state;
- `migrateTheme` for every row of the table in section 4;
- the window arithmetic: inside, at `from` (on), at `until` (off),
  crossing midnight both sides, `from == until` refused;
- the override: set by `toggleNight()` in schedule mode to the opposite
  of the window; a second tap clears it and the schedule shows again (a
  tap never sets the same override twice); cleared by a change to Night,
  From or Until and left by a change of style; cleared at the next
  boundary (the fake clock stepped past it, `refreshNight()` through the
  Timer's handler, not called directly), kept across a restart (stored
  and read back), expired on load;
- the words: the two rows of section 5's table ("On until 07:00" for
  the 18:00 tap, "Off until 19:00" for the 20:00 tap), the expiry time
  never shown, and the no-override summary;
- the stored keys follow each property, as `test_theThemeIsKept` does;
- the two new floors in both night palettes: every trace's luminance at
  or under the text's, `gaugeReadout` at or under the text's; the
  existing floors (3.5:1, 38 apart, warm) over the new series;
- the Dark exemptions pinned (`test_darkKeepsItsTraceColors` as is);
- the danger-hue test stays a Dark test (in Night, Red every color is
  near the danger hue by design).

`QmlTest.settings`: the Night and Night style buttons set and highlight;
the schedule fields store a valid time and refuse a bad one with the
words; the Scheduled row shows the fields and Off hides them; the list's
summary words.

`QmlTest.dashboard`: the switch flips the mode in On/Off mode and sets
the override in schedule mode; `highlighted` follows `nightActive`; both
loaders place it (desktop and upright phone), `shortLayout` moves it.

`QmlTest.phone`: the existing target sweep covers the switch at 48;
one new check that the title is not elided at 384 wide with the switch
present (`implicitWidth <= width`).

`QmlTest.gauges`, `QmlTest.kinds`, `QmlTest.liveview`: the rename to
`paletteNames` and the three palette keys; no behavior change.

`BridgeTest.WindowTheme*`: unchanged.

Mutants for the handover (each must be killed): `<` for `<=` at `from`;
the midnight crossing inverted; the override never clearing at the
boundary; a second tap setting the override again instead of clearing
it; the clearing on a change of Night, From or Until removed (one
mutant each); the override not stored; the migration rows swapped; the
Timer removed; the activation hook removed; the luminance floor
loosened; the switch's override set in On mode; `from == until`
accepted; the words showing the expiry instead of the next change.

Manual, on the test phone in the dark: both styles on the Dashboard,
the switch's two placements, a tap and a second tap under a schedule, a
schedule boundary with the app open and one with it in the background
then reopened, the Windows title bar at a boundary. Evidence in a
session sheet under `docs/bench-sessions/`.

## Alternatives considered

- **Keep three themes, rename "Low glare".** The smallest change, but a
  schedule and a one-tap switch then need to know which of two themes
  "night" means, and the user picks a theme in one place and a schedule
  in another that overrides it. Separating the occasion (night) from the
  style (amber or red) is what makes the switch one tap.
- **One night palette with a hue parameter** (amber rotated to red).
  Fewer literals, but every floor would still need asserting per
  output, and the hand-tuned surfaces and roles are what passed the
  phone check; a generator would move them. Two palettes, one structure.
- **Generate the trace palette by search.** The search (appendix) finds
  sets far apart in RGB, but their members include olive and gray-brown
  colors that read cold or muddy on the phone. Hand-kept colors with the
  three offenders replaced keep what worked.
- **Follow the system's dark mode.** Qt exposes the light or dark
  scheme (`Application.styleHints.colorScheme`, Qt 6.5 and later), and
  the app is dark already; a Light look later can follow the scheme.
  Android's night mode (`UiModeManager` in the Android developer
  reference, with its custom start and end times) schedules the system's
  dark theme, not a palette of the app's, and reading it is JNI for no
  gain while the app is dark either way.
- **Sunset and the light sensor.** Both are the right long-term
  triggers; sunset needs location, the sensor is phone-only. The mode is
  an append-only string so they join later.
- **A tap in schedule mode switches the mode to On or Off.** Simple, but
  it silently deletes the schedule the user set up, and the next evening
  nothing happens; an override that ends at the boundary keeps the
  schedule and still gives the tap its effect now.
- **The switch in the top bar (Main's header).** It is reachable from
  every page, but on the phone the bar already elides the link pill to
  "Ses..." beside `Start log` and `Disconnect`; there is no room, and the
  Dashboard is where a night driver is.
- **The switch in the Dashboard's title row on the phone.** Fits at the
  default font, elides the title at larger ones (section 6).
- **An in-app dimmer** (a dark overlay, or Android's per-window
  brightness). Not a theme: the OS brightness does it, and an overlay
  lowers contrast as much as glare. Could join Night later as "Dim the
  screen" on Android, which has the per-window call.
- **A popup time picker.** The controls' popups draw outside the zoom
  (ADR-0057), the native pickers need JNI on Android, and a masked field
  is one line.
- **Compute the next boundary and arm a one-shot timer.** Exact, but a
  sleep, a clock change or a time-zone change needs re-arming logic; a
  30 s tick comparing two times costs nothing and has no such path.

## Consequences

- Settings' Display page loses the theme row and gains three; "Low
  glare" leaves the strings. CHANGELOG: one "Changed" line (the themes
  become Night mode with a style, the stored theme carried over) and one
  "Added" (the schedule and the switch).
- Tests churn: the writable `Theme.theme` goes; the six test files that
  set it move to `night` and `nightStyle`, `themeNames` to
  `paletteNames`. Listed in section 9.
- ADR-0057 section 9's "low-glare and night themes" are the night styles
  (its dated note), and the phone shots in the README name the amber
  style.
- The phone's upright Dashboard gives 28 dp to the switch until the
  button hierarchy frees the title row.
- No core, bridge or C ABI change. `havne-cli` has no display, so no
  gap (ADR-0057 section 8's reasoning).

## Decisions

Decided 2026-11-08; the sections above describe the decided form.

1. **Names.** "Night" with the styles "Amber" and "Red". "Low glare"
   goes: the occasion is the mode, and the style says its color.
2. **Default night style.** Amber: the more legible, the cabin's usual
   color, and warning and danger still read as colors.
3. **The Look control.** Stored now and shown only when a second look
   exists.
4. **The switch's slot on the phone upright.** The end of the link line,
   costing 28 dp. The title row fits it at the default font and elides
   the title at larger ones.
5. **A tap in schedule mode.** An override until the next boundary, not
   a change of the mode.
6. **Schedule defaults and tick.** 19:00 to 07:00 and a 30 s tick.
7. **The trace rule's scope.** The two night palettes only. Dark's
   near-white trace stays, since Dark is the day look and
   `test_darkKeepsItsTraceColors` pins it.
8. **The readout.** `gaugeReadout` is the text color in both night
   palettes: the difference is 1 % and the rule reads cleanly.
9. **Palette keys.** `nightAmber` and `nightRed` with `paletteNames`:
   the keys say what they are.
10. **The override across restarts.** Stored with its expiry.
11. **A desktop shortcut.** None. If one is wanted, it is a key no page
    uses, documented beside the zoom keys.
12. **The ADR number.** 0058, with ADR-0057 section 9 amended by a dated
    note, not rewritten.

## Appendix: how the numbers were measured

- Relative luminance and contrast per WCAG 2.x: sRGB channels
  linearized with the 0.04045 knee, L = 0.2126 R + 0.7152 G + 0.0722 B,
  contrast (L1 + 0.05) / (L2 + 0.05). The same formula as
  `tst_theme.qml`'s `lum` and `contrast`, so the ADR's floors and the
  test's agree.
- Distance between two traces: Euclidean in 8-bit RGB (the test's 38
  floor). Hue: HSV, degrees.
- The palettes are the literals in `app/qml/Theme.qml` at bbc9050.
- Three short Python scripts, not in the repo and re-creatable from
  this description: one over today's three palettes (every trace's
  luminance against the text's, contrast on bg and surface, least
  pairwise distance), one exhaustive search of the warm range under the
  floors (5751 amber and 3814 red candidates; greedy farthest-first
  sets reach least distances of 70.3 and 65.3), and one over the
  proposed sets (every floor checked; every row within them). The
  figures were recomputed independently from `Theme.qml` on 2026-11-08
  and matched.
- Measured: every luminance, contrast and distance above, and from the
  phone screenshots the header's widths in the morning build (the title
  about 96 dp, the two buttons 119 dp each, before the compact header).
- Estimated, not measured: the compact header's button widths (about 92
  and 50 dp, from the compact rule's text width plus 12) and so the
  title row's spare width in section 6; and the remark that the searched
  trace sets read olive or muddy, a judgment of the hex values, not a
  phone check. Neither estimate changes a number a test asserts.
