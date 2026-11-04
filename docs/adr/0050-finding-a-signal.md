# ADR-0050: Finding a signal among many: a group per def, one filter

- **Status:** Accepted. Built on 2026-11-03.
- **Date:** 2026-11-03.
- **Builds on:** [ADR-0016](0016-signal-definition-layout.md) (the
  def's layout and its reserved room);
  [ADR-0039](0039-full-service01-coverage.md) (every linear Service 01
  field in one `obd2` set, and the plot rank);
  [ADR-0049](0049-recordings-in-core.md) (`events.log`'s `signal`
  lines, the reader, the viewer).
- **Amends:** ADR-0016's display block, which gives its second reserved
  byte to the group (an appendix there); ADR-0049 section 4's `signal`
  line, which gains the group as a sixth field (a note there).

## Context

The `obd2` set holds every linear Service 01 field, so Live's list and
the Recordings page's signal rows run to well over a hundred rows on a
car that answers most of them. Live's list hides what the car lacks,
which shortens it, but a signal is still found by scrolling, and the
viewer lists every signal a recording holds. A def says what it is
(its name and unit) but not what part of the car it belongs to, so
neither page can offer anything better than the whole list.

## Decision

1. **A group per def, as a code.** `ecuio_signal_def_t` gives the
   second byte of its display block (offset 90) to `group`, one of an
   append-only list, `ECUIO_SIGNAL_GROUP_*` in `signals.h`:

   | Code | Name | For |
   |---|---|---|
   | 0 | Other | anything not placed, and `uds-generic`'s DIDs |
   | 1 | Engine | speed, load, timing, torque, coolant and oil |
   | 2 | Air | intake and ambient air, pressures, flow, throttle and pedal |
   | 3 | Fuel | trims, pressures, level, rate, injection, commanded lambda |
   | 4 | Oxygen sensors | every O2 sensor's reading and trim |
   | 5 | Emissions | EGR, evaporative purge and pressure, catalyst temperatures |
   | 6 | Vehicle | road speed, the odometer |
   | 7 | Electrical | the battery's voltage, a hybrid battery's life |
   | 8 | Diagnostics | the MIL's time and distance, the counts since a clear |

   `ecuio_signal_group_name(code)` gives the name, and "" for a code
   this core doesn't know, which a UI shows under Other. Every value is
   valid, as the plot rank's is, so the validator stops checking that
   byte; the layout, its size and `ECUIO_SIGNAL_SET_LAYOUT` are
   unchanged, and a set that leaves it zero groups everything under
   Other. The names are core's, like the names and units of the defs,
   so the app and `havne-cli` say the same.
2. **The built-in sets are grouped.** `obd2` places each def by its
   PID, the fields of one PID together, with one exception: PID 4F's
   largest intake pressure goes under Air, beside its O2 maxima's
   PID. The mock's defs are grouped the same way. `uds-generic` leaves
   its DIDs under Other: a DID says nothing of what it reads.
3. **Recordings carry it.** An `events.log` `signal` line ends with the
   group's code, a sixth field after the rank. The reader takes five
   or six fields, so a recording without it opens with every signal
   under Other, and fills `group` in `ecuio_recording_signal_t` (the
   first of its two reserved bytes, offset 54). Nothing released wrote
   or read format 2 before this, so no released reader meets the sixth
   field.
4. **One filter for Live and the viewer.** Both pages show the same
   filter, one row high (`SignalFilterBar.qml`): a text field, and a
   **Filter** button whose popup holds **Plotted only**, a box for each
   group the list holds (in code order, when it holds more than one)
   and **Clear**. On Live it sits in the title's row, so a short page,
   whose list and chart already share every pixel, loses no height to
   it; the status line then counts the rows shown of the rows the
   session has. A row shows when every word typed is found, ignoring
   case, in its name or its unit as shown (psi under imperial units),
   when it's plotted if **Plotted only** is on, and when its group is
   chosen or none is. The rule and its state are one class,
   `SignalFilter` (`app/signalfilter.h`): Live's proxy model holds one
   and the Recordings page makes its own, so both filter alike. The
   filter hides rows and changes nothing plotted.
5. **`havne-cli` prints the group.** `stream`'s `signals` line and
   `view`'s and `info`'s `signal` lines carry `group`, the name. The
   CLI's output is JSON lines, so finding a signal there is the
   reader's filter (`jq`, `grep`), not a flag of its own: that's the
   one difference from the app, and the group is what both give.

## Alternatives considered

- **A group as text** (16 bytes of the def's reserved room): an
  imported set could bring its own groups. It takes most of the room
  ADR-0016 kept for growth, and chips would come in whatever order and
  wording a set used. A set imported in bulk (v0.5.0) maps its groups
  onto the list instead, and the list grows by appending.
- **Groups from the unit or the name** (every temperature together):
  no def change, but a coolant temperature and a catalyst's belong to
  different parts of the car, and units already match in the text
  filter.
- **A group per PID in the app:** only `obd2` would be grouped, and the
  CLI and a recording would know nothing of it.
- **Chips under the text field:** every group in view at a tap, but a
  second row of height. On a 330 px page Live's list and chart already
  take all of it, and the chart went past the bottom.

## Consequences

- A long list narrows in a keystroke or a tap, on both pages, and the
  same way on each.
- A def's second display byte is spoken for; one display byte stays
  reserved.
- An unknown code from a newer set or recording shows under Other on
  an older UI, which is right enough to keep the row findable.
- The group's names are English, as the defs' names are.
