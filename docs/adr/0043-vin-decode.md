# ADR-0043: VIN decode on the device: the model year and check digit by 49 CFR 565.15, the make by NHTSA vPIC's manufacturer codes, built into core and given alike by the app and havne-cli

- **Status:** Accepted.
- **Date:** 2026-11-01. Implemented with this record.
- **Builds on:** [ADR-0021](0021-vehicle-identification.md) (the VIN,
  asked once per channel at connect, and the identity report);
  [ADR-0027](0027-vehicle-profiles.md) section 3 (the VIN stays on the
  device, which core's `ecuio_identity_vin` takes from the report);
  [ADR-0031](0031-command-line-client.md) (havne-cli, a front end with the app's
  reach, schema 1's lines).

## Context

Havne reads the VIN at every connect and shows it as text. The VIN
also says things a user wants to see without decoding it by hand: the
model year, the maker, and whether it was read whole. Two facts shape
how to decode it:

- **The VIN must not leave the device** (ADR-0027). NHTSA's vPIC
  service decodes VINs online, but sending each car's VIN to a web API
  is the leak ADR-0027 avoids. vPIC also publishes its database for
  download (https://vpic.nhtsa.dot.gov/downloads/, monthly), with no
  license or registration requirement: its FAQ calls it free for public
  use, and NHTSA's terms of use call the site's information public, to
  be "distributed or copied". The download's own page limits it to VIN
  decoding, which is this use.
- **The rules are federal regulation.** 49 CFR 565.15 sets the VIN's
  sections: the manufacturer code in positions 1-3 (six characters for a
  low-volume manufacturer, a 9 in position 3 joined by positions 12-14),
  the check digit in position 9 with its tables of values and weights,
  and the model year in position 10, a cycle of 30 codes placed by
  position 7. They need no data at all.

Havne has two front ends of the same standing: the app (Windows,
Linux, Android, and iOS, which builds and has run only on the Simulator)
and `havne-cli`. A feature one has and the other lacks is a divergence,
so both get the decode.

## Decision

### 1. The rules, from 49 CFR 565.15

Core decodes what the regulation defines, citing its text (eCFR,
current as of 2026-10-29):

- **Check digit** (565.15(c)): each character's value by Table III
  (digits their own; letters A-H 1-8, J-N 1-5, P 7, R 9, S-Z 2-9), times
  its position's weight by Table IV (8, 7, 6, 5, 4, 3, 2, 10, 0, 9 to 2),
  summed, modulo 11, a remainder of 10 written X. Table VI's sample VIN
  checks the rule (`VinTest.TheRegulationsSampleDecodes`). A digit that
  doesn't match is reported, never refused: the regulation binds
  vehicles for sale in the US, and other markets may not use it.
- **Model year** (565.15(d)(1), Table VII and its note): the codes run
  A-H, J-N, P, R-T, V-Y, 1-9, 30 of them; 0, U and Z are no year. A
  digit in position 7 places the code in 1980-2009, a letter in
  2010-2039. The note covers passenger cars, and multipurpose vehicles
  and trucks of 10,000 lb GVWR or less, built to it. A VIN from before
  the note's rule may carry a letter in position 7 and read 30 years
  late, so the caller passes the latest model year possible (this year
  plus one in both front ends); a year past it is taken 30 years back
  and flagged.
- **Manufacturer code** (565.15(a)): positions 1-3, or 1-3 and 12-14
  when position 3 is a 9.
- **Not a VIN:** anything but 17 digits and capital letters other than
  I, O and Q is refused. `ecuio_identity_vin` takes any 17 letters and
  digits, so a VIN the decode refuses is still logged as the car gave it.

### 2. The makes, from vPIC's standalone database

`scripts/vpic_wmi.py` reads the plain SQL release's zip (no database
server needed) and writes `core/src/vin_wmi_table.h`, committed: each
manufacturer code vPIC lists for a vehicle other than a trailer, with
the makes vPIC names under it, sorted for a binary search, and a header
naming the release and the zip's SHA-256. From `vPICList_lite_2026_10`
that is 3,373 codes and 2,505 distinct labels, about 134 KB of source.

- **Trailers are left out** (9,641 of vPIC's 13,014 codes): a trailer
  has no ECU to read.
- **The label is the make, not the manufacturer:** vPIC's manufacturer
  names are legal names, mostly in capitals, some with broken non-ASCII
  bytes; its make names read as a user knows them. A code several makes
  share lists them all ("Datsun, Infiniti or Nissan"), since the code
  alone can't say which.
- **Codes with an O** (vPIC holds two) are left out, as no VIN can carry
  them. The script refuses non-ASCII makes and a code listed twice, and
  core checks the table at compile time: sorted, unique, the VIN
  alphabet's, three or six long.
- **Refresh** is a rerun of the script on a newer release and a commit;
  the table never comes from the network at build or run time.

vPIC lists the codes of vehicles made for sale in the US, so a code
used only in other markets reads as unknown: the decode says so rather
than guess.

### 3. The C ABI: `ecuio/vin.h`

A public header of its own: `ecuio_vin_decode(vin, latest_model_year,
&info)` fills `ecuio_vin_info_t` (32 bytes: flags
`ECUIO_VIN_CHECK_MATCHES` and `ECUIO_VIN_YEAR_EARLIER`, the model year,
the check digit it works out to, the code and its length, reserved
room); `ecuio_vin_make(code)` returns the make, "" for a code vPIC
doesn't list; `ecuio_vin_source()` names the vPIC release. No session
is needed: a program decodes a VIN it typed, read or got from
`ecuio_identity_vin`.

### 4. Each front end

| | The connected car's VIN | A typed VIN |
|---|---|---|
| **The app** (every platform: one QML app on one core) | A `VIN:` line in the activity log and a recording's `events.log`, beside the identity line that holds it, once a session | Settings, Vehicle, **Look up a VIN**: decoded as it's typed, never stored |
| **havne-cli** | `stream`'s `identity` line per ECU once complete: each item (name, request, outcome, text) and `vin`, the decode | `havne-cli vin <VIN>`: a `start` line and a `vin` line; not a VIN is exit 2 |

Both front ends say the same thing from the same calls: the app's line
is `SessionBridge::vinSummary`, the CLI's fields `cli/vin_line.h`. Both
drop spaces and take lower case in a typed VIN, and pass this year plus
one as the latest model year. The identity line is new to the CLI as
well: the app has logged each ECU's identity since ADR-0021, and the
CLI had no line for it.

## Alternatives considered

- **vPIC's online API:** decodes more (model, trim, engine), but every
  lookup sends the VIN to a server. Refused under ADR-0027.
- **vPIC's whole decoder offline:** its patterns reach model and trim,
  but they are most of a 337 MB SQL dump and its stored procedures, a
  database engine's worth of work for a reader that shows the year and
  the make.
- **Extracting the table at build time:** every build, CI's included,
  would download 73 MB first, and a release could change with no commit
  showing it. The committed table is reviewed like code.
- **Community VIN libraries:** the one reviewed for this record draws
  its manufacturer list from Wikipedia (CC BY-SA, a share-alike license
  for the data, unlike Havne's MIT), and its counts disagreed with
  themselves. vPIC is the primary source.
- **The manufacturer's legal name as the label:** what the regulation
  says a code identifies, but vPIC's names are inconsistent in case and
  encoding; the make reads better, and the code is shown beside it.

## Consequences

- **Good:**
  - The year, the make and a check on the reading, on every platform and
    both front ends, with nothing sent anywhere.
  - The rules cite the regulation and the data its publisher; the table's
    release and hash are in its header.
- **Bad:**
  - About 110 KB of data in core (MSVC's object, 2026-11-01), and a
    refresh is a manual step (a release a year or two old still knows
    nearly every car on the road).
  - Codes from outside the US market read as unknown.
  - The model year is the note's rule: a heavy truck's or an older VIN's
    year may be off by 30 within the bound; the flag says when the bound
    moved it.
  - The CLI's identity line puts the VIN and the calibration IDs on
    stdout, as the app's log does: output shared in a bug report needs
    the same care as an `events.log`.
