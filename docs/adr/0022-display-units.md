# ADR-0022: Display units — metric or imperial, converted in core at the edge of the UI

- **Status:** Accepted.
- **Date:** 2026-10-24. Implemented with this record.
- **Builds on:** [ADR-0001](0001-qt-free-core-c-abi.md) (rule 2: the UI
  receives final display values; core owns names, units and decimal
  places); [ADR-0016](0016-signal-definition-layout.md) (each def's
  `unit` string and `decimal_places`); [ADR-0017](0017-session-recording.md)
  (what a recording keeps).

## Context

Every def carries its unit as a display string, and the sets use
metric: km/h, degrees C, g/s, kPa. A user in the US reads a
speedometer in mph and a temperature gauge in degrees F, so the
pages need both. Rule 2 rules out the QML doing the arithmetic: a
second place that turns a number into what the user sees would drift
from the first, and the Dashboard's scales and zones would each need
the formula.

Three things pull on where the conversion sits. The value a session
decodes is what the ECU sent; a recording is evidence and must keep
it. The strip chart holds about 12 s of history per signal, and a
change of units shouldn't empty it. And signal sets come from three
places (the built-in mock and `obd2` sets, `uds-generic`'s per-session
set, later plugins), so a per-set table of imperial units would have
to be kept in step with every one of them.

## Decision

### 1. One function in core, keyed by the unit string

`ecuio_display_convert(unit, decimal_places, value, units, out)`
(`signals.h`) takes a decoded value with its def's unit and decimal
places and a unit system (`ECUIO_UNITS_METRIC` or
`ECUIO_UNITS_IMPERIAL`), and writes an `ecuio_display_value_t`: the
value, the unit it's in, its decimal places, and whether anything
changed. Under imperial, km/h becomes mph, degrees C become degrees F,
kPa becomes psi, g/s becomes lb/min and km becomes mi; psi and lb/min
are coarser, so they gain a decimal place (capped at 3, the
validator's limit). Every other unit, and every unit under metric,
passes through unchanged.

The conversion is chosen by the unit string, matched whole. Any set
that spells a unit the way the built-in sets do converts with no
table of its own; one that spells it differently passes through
unconverted, visibly, rather than wrongly.

### 2. The bridge stores decoded values and converts on the way out

`SignalListModel` keeps each row's decoded value and its history ring
as decoded. `ValueRole`, `UnitRole`, `DecimalsRole` and `chartData()`
convert through core when read. A change of `QtTuneBridge.unitSystem`
emits `dataChanged` for those roles over every row and one
`valuesUpdated`, so the list, the gauges and the whole chart history
redraw in the new units at once, connected or not.

The Dashboard's scale ends and zones are written once, in the decoded
units, and shown through `Bridge.displayValue(unit, v)` and
`Bridge.displayUnit(unit)`. The one exception is the speedometer's
end, which picks a round number in each system (260 km/h, 160 mph)
because 161.6 mph would put its labels off the 20 mph steps. A change
of units clears the Dashboard's held peaks, which were taken in the
old units.

### 3. Recordings keep what was decoded

`SessionRecorder` writes the decoded value and the def's unit, as
before. A recording reads the same whatever the screen showed, and a
replay of it converts like a live session.

### 4. The choice is a preference, kept between runs

Settings' Units row sets `Bridge.unitSystem`. `Main.qml` keeps it in
QML `Settings` (category `ui`, beside the zoom) and applies it at
startup. The bridge ignores any value but metric or imperial, so a
corrupt stored value leaves the default.

## Alternatives considered

- **Convert in QML.** One formula per unit in `SignalHub` or a
  `Units` singleton would be shorter, but it is exactly the second
  display path rule 2 exists to prevent, and the chart, the list and
  the gauges would each have to call it.
- **Imperial units in the defs.** A second `unit_imperial` field per
  def (the reserved display bytes have room) would let a set choose
  its own imperial form, but every set, `uds-generic`'s built per
  session included, would have to fill it, and the conversion factors
  would still live somewhere. Keying by unit string needs nothing from
  a set.
- **Convert at decode time.** Converting before staging would make the
  recording follow the screen and the history the unit in force when
  each sample arrived, so a change of units would leave the chart
  mixed.

## Consequences

- One place holds the factors, and `SignalsTest.DisplayConvert*` pins
  each one exactly. `BridgeTest.UnitSystem*` pins the round trip
  through the model, the chart's history and the recording;
  `QmlTest.dashboard` and `QmlTest.settings` the gauges' scales and the
  Units row.
- A unit spelled differently (`degC`, `kph`) doesn't convert. The
  built-in sets share their spellings (`SignalsTest.MockAndObd2ShareDisplayNamesAndUnits`),
  so this only bites a set written by hand, and it shows as metric,
  not as a wrong number.
- Per-signal overrides (one quantity in another system) stay on the
  roadmap; they would pass a unit system per row instead of one for
  the model.
- `ecuio_display_value_t` is ABI: 32 bytes, pinned by static asserts
  and `test_c_abi.c`, with six reserved bytes.

## Appendix — the default system (2026-10-25)

The app starts in imperial until the Units row is used: `Main.qml`'s
`Settings` default for `unitSystem` is imperial, and a stored choice
wins over it. Only the display default moved. The bridge still starts
in metric, which is what core decoded, so a bridge without the app's
window (`test_bridge`, and any value the window can't read back) shows
decoded units, and recordings keep the decoded value and unit as §3
decided.
