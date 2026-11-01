# ADR-0045: Inspection readiness: the car's monitors and light judged by the federal OBD inspection rule, 40 CFR 85.2222, for the VIN's model year, in core and alike in the app and havne-cli

- **Status:** Accepted.
- **Date:** 2026-11-01. Implemented with this record.
- **Builds on:** [ADR-0037](0037-reading-trouble-codes.md) (the
  diagnostics report: the MIL and the readiness monitors, PID 01);
  [ADR-0043](0043-vin-decode.md) (the model year from the VIN);
  [ADR-0031](0031-command-line-client.md) (havne-cli, schema 1's lines).

## Context

Diagnostics shows each ECU's monitors and whether each is complete. The
question a driver brings is shorter: would the car pass an emissions
inspection today, or does it need more driving first (after a clear, or
a battery change, the monitors start over)?

The federal rule for an OBD inspection is 40 CFR 85.2222
(https://www.ecfr.gov/current/title-40/chapter-I/subchapter-C/part-85/subpart-W/section-85.2222;
its text as of 2026-11-01):

- **(c)(1):** a car with incomplete monitors is sent away to be driven,
  and fails on a second visit still incomplete;
- **(c)(2):** the exception: a model year 1996 to 2000 car with two or
  fewer unset monitors, or a 2001 or newer one with no more than one,
  may pass without being driven;
- **(d)(1):** a commanded MIL fails the car.

States run their own programs on top of it, and some are stricter
(California's, 16 CCR 3340.42.2, sets its own limits); Havne knows no
car's state and judges by the federal rule only.

## Decision

### 1. Core judges, both front ends show it

`ecuio_diag_inspect(reports, count, model_year, &out)` in `core.h`
takes every channel's report and the car's model year (0 when none is
known) and fills an `ecuio_inspection_t` (24 bytes): the verdict
(`ECUIO_INSPECTION_*`), the incomplete monitors' count and ids, how many
the model year allows, and the reports counted. The reports are one
car's: a monitor is incomplete when any ECU that has it says so, and
the MIL is on when any ECU commands it. A report counts once PID 01
has answered.

The verdicts, in the order checked:

| Verdict | When |
|---|---|
| `READING` | a report asked is still being read |
| `UNKNOWN` | no ECU answered PID 01 |
| `NOT_COVERED` | a model year before 1996 |
| `MIL_ON` | an ECU commands the MIL: (d)(1) fails it |
| `READY` / `NOT_READY` | the incomplete count against the model year's allowance, (c)(2) |
| `YEAR_NEEDED` | no model year and exactly two incomplete: 1996-2000 allows them, 2001 on doesn't |

With no model year, one incomplete passes both rules and three fail
both, so only two needs the year.

### 2. Every listed monitor counts

The rule doesn't say which monitors count beyond those that apply to
the car (40 CFR 86.1806), and PID 01 lists only those the ECU has.
Havne counts every listed monitor that reads incomplete, continuous or
not. Counting one an inspection would skip can only make the verdict
stricter (Texas, for one, counts the non-continuous monitors:
https://www.dps.texas.gov/section/vehicle-inspection/faq/ready-or-not).

### 3. The model year is the VIN's

Both front ends decode the first channel's VIN (ADR-0027 section 3's
vehicle) with this year plus one as the latest, as ADR-0043's lines do.
Identification runs before diagnostics, so the year is there when the
monitors are; without a VIN it is 0.

### 4. Each front end

| | Shows |
|---|---|
| **The app** (every platform) | Diagnostics: an **Inspection readiness** card above the ECUs once every read has completed: one sentence (the verdict, the incomplete monitors by name, the model year's allowance) and the rule's name and reach |
| **havne-cli** | `stream` writes an `inspection` line after the `diagnostics` lines of a completed read: verdict (`ready`, `not_ready`, `mil_on`, `year_needed`, `not_covered`), `model_year` (null with none), `incomplete` (the monitors' names), `allowed` (null with no year), `reports`, `rule` |

Neither writes anything while a read is under way or when no ECU
answered PID 01.

## Alternatives considered

- **A state's rule:** the stricter programs differ from each other, and
  the car's state is unknown; one federal rule, named, beats a guess.
- **Each ECU judged alone:** an engine ECU ready and a transmission ECU
  not would read as two answers; an inspection judges the car.
- **The model year typed by the user:** the VIN gives it on every car
  that answers 09 02; a typed one can still be added if a car without a
  VIN needs it.

## Consequences

- **Good:**
  - One line answers whether the car needs more driving before an
    inspection, the same in the app and the CLI, offline.
  - The rule and its source are named wherever the verdict is shown.
- **Bad:**
  - A state program stricter than the federal rule can fail a car this
    calls ready.
  - The MIL's bulb check ((d)(4)) and a station's other tests are out of
    reach: the verdict is the OBD readiness part only.
  - A misread model year (a VIN's year code outside the car's own cycle)
    moves the allowance; the year shown beside the verdict makes that
    visible.
