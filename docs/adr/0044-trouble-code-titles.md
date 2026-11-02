# ADR-0044: Trouble-code titles on the device: SAE J2012's names for the generic powertrain and network codes, from Wal33D's MIT list, built into core and given alike by the app and havne-cli

- **Status:** Accepted.
- **Date:** 2026-11-01. Implemented with this record.
- **Builds on:** [ADR-0037](0037-reading-trouble-codes.md) (the diagnostics
  report: the three code lists and the freeze frame's code);
  [ADR-0031](0031-command-line-client.md) (havne-cli, schema 1's lines);
  [ADR-0043](0043-vin-decode.md) (reference data built into core, the
  same in both front ends).

## Context

The Diagnostics page and havne-cli's `diagnostics` line show each
trouble code as it is written ("P0301"), which a user then looks up
elsewhere. SAE J2012 names the generic codes; a name beside each code,
offline, saves that step.

Five open lists were compared on 2026-11-01, against each other and
against J2012's April 2002 edition, which 40 CFR 86 incorporates by
reference and Public.Resource.Org publishes
(https://law.resource.org/pub/us/cfr/ibr/005/sae.j2012.2002.pdf; the
scanned pages for P0040-P0094 and P0410-P0464 transcribed by hand):

- **Wal33D/dtc-database** (MIT, https://github.com/Wal33D/dtc-database):
  9,415 generic titles in a later edition's wording. On the 110 codes
  transcribed, each title names the same part and fault as the 2002 page;
  38 word them differently ("AIR System" for "Secondary Air Injection
  System", a sensor number added). It calls itself SAE J2012's
  generic codes but names no source.
- **OBDex** (CC0): shares much of Wal33D's wording but contradicts it on
  76 codes, naming another side, bank, sensor, cylinder or polarity, in
  runs of shifted rows (each cylinder-deactivation code one cylinder
  off); about 280 body and chassis codes appear in no other list, and its
  longer descriptions read as generated. Rejected.
- **python-OBD** (GPL-2.0): never contradicts Wal33D on those points.
  Used only as a check, so nothing of it ships.
- **mytrile** (MIT): shifted one row from P0410 to P0453 (its P0420 is
  P0419's title). Rejected.
- **Gershy13** (MIT): 244 codes in older wording. Too few.

## Decision

### 1. Wal33D's generic titles, in SAE's powertrain and network ranges

Havne names the codes of J2012's ranges that SAE controls, of two
letters: P0, P2 and P34-P39 (powertrain), U0 and U3 (network). Left
out:

- **A manufacturer's ranges** (P1, P30-P33, U1, U2): their meaning is
  each maker's, so a generic title would mislead.
- **Body and chassis codes** (B, C): Services 03, 07 and 0A report
  emission-related codes, which in practice are powertrain and network
  codes, and the lists disagree most over B and C: Wal33D's C0033-C0036
  name the right front wheel where OBDex names the left, with nothing to
  settle it.

`scripts/dtc_titles.py` reads Wal33D's `data/source-data/p_codes.txt`
and `u_codes.txt` and its `LICENSE` at one commit and writes
`core/src/dtc_titles_table.h`, committed: the commit, each file's
SHA-256, the license, then each code and title sorted by code. From
commit `04c43d7` (2026-02-16) that is 8,582 titles; 36 lines in a
manufacturer's range and one whose title was itself a code
("P3400 - P34C8", above P3400's own line) are left out. Curly quotes
become straight ones and an en dash a hyphen, so core's strings stay
ASCII; anything else outside ASCII, a code listed twice, or a line that
isn't `CODE - Title` stops the script. Core checks the table at compile
time: sorted, unique, every title text. A refresh is a rerun on a newer
commit and a commit here.

No urgency is given: none of the lists rates one, and a made-up rating
would read as advice.

### 2. The C ABI: four calls in `core.h`

Beside `ecuio_dtc_format`: `ecuio_dtc_parse(text, &code)` is its
inverse ("P0301" to 0x0301; upper case, a first digit 0-3);
`ecuio_dtc_title(code)` returns the title, "" for a code outside the
list; `ecuio_dtc_title_source()` names the list and commit;
`ecuio_dtc_title_notice()` returns its MIT notice whole.

### 3. The license's notice ships with the titles

MIT asks that its copyright and permission notice be included in every
copy or substantial portion. The titles are most of Wal33D's generic
list, so:

- core compiles the notice in (`ecuio_dtc_title_notice`), so each binary
  that holds the titles holds it too: the app on every platform,
  havne-cli and the core library;
- the Windows zip's `THIRD_PARTY_NOTICES.txt` carries it, and core's
  packages install `core/THIRD_PARTY_NOTICES.txt` as
  `share/doc/havne-core/THIRD_PARTY_NOTICES` (with vPIC's, ADR-0043);
- the app shows it on request, under Diagnostics' lookup.

*Amended 2026-11-01:* the app names neither the list nor its license on
Diagnostics: the page is for the car's codes, and the notice is met by
the binaries and the notices files above. The release notes name both
sources under "Third-party data". The lookup and the titles beside the
car's codes stay.

Havne's own license stays MIT.

### 4. Each front end

| | The car's codes | A typed code |
|---|---|---|
| **The app** (every platform) | Diagnostics: each titled code on a line under its list, and the freeze frame's title beside its code | Diagnostics, **Look up a code**, with or without a session |
| **havne-cli** | the `diagnostics` line's lists add `titles` beside `codes` (null where the list has none), and the freeze frame's `title` beside its `code` | `havne-cli dtc <code>`: a `start` line and a `dtc` line (code, title or null, source); not a code is exit 2 |

The app's saved report before a clear is the CLI's lines
(`cli/diagnostics_line.h`), so it carries the titles too. The activity
log's diagnostics line keeps the codes alone: one line per ECU stays
readable, and the page and the report hold the names. Both lookups drop
spaces and take lower case.

## Alternatives considered

- **OBDex:** CC0 and the longest list, but its shifted rows contradict
  the others, and its added codes and prose have no source.
- **python-OBD's list:** agrees with Wal33D, but its GPL-2.0 would bind
  the code that ships it; kept as a check only.
- **Manufacturer lists** (Wal33D holds 33 makes'): no stated source and
  no way to check them here; a wrong title for a maker's own code would
  be worse than none.
- **Titles looked up online:** the page would need a network and a
  service's terms; the codes, unlike the VIN, are not personal, but an
  offline reader shouldn't depend on one.
- **A packed table** (one string of titles, offsets beside the codes):
  about 70 KB less, but MSVC refuses a string literal past 64 KB, so it
  would be an array of characters, unreadable in review.

## Consequences

- **Good:**
  - The car's codes read as names on every platform and in both front
    ends, with nothing sent anywhere.
  - The list, its commit and its notice are in the source and in every
    download; the ranges left out are the ones a generic title would get
    wrong.
- **Bad:**
  - About 566 KB of data in core (MSVC's object, 2026-11-01).
  - The titles are J2012's names in a later edition's wording, checked
    against the 2002 edition's pages on 110 codes and against the other
    lists elsewhere, never against the current edition, which isn't
    public.
  - A code defined after the list's commit, a manufacturer's code and a
    body or chassis code show no title.
