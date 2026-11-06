# Plugins

A plugin's data tier is a package of signal files
([docs/signal-files.md](../docs/signal-files.md)) with one manifest
that names the plugin, the cars it's for, the ECUs its files read and
its variants, each an ordered list of its files merged by signal key
([ADR-0052](../docs/adr/0052-plugin-framework.md)). Core loads a
package from its folder or packed into one file, and each variant
registers as the profile `plugin.<id>.<variant>`, which a URI's
`?profile=` names.

This folder holds only the example, `example-sim/`: made-up DIDs on two
modules (the engine at `7E8`, a transmission at `7E9`) that the
simulated ECU answers from the plugin's own definitions, with no car.
Its `base` variant extends `obd2` with five signals (one an
enumeration, one computed); `my2019` merges a second file on top,
which replaces two of them and adds one. `fixtures/values.json`
lists the value each signal should read back. A real make's plugin
stays with its author, never here.
