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

## In the app

The app takes a plugin in its packed form, one file named
`<id>.havne-plugin.json`: the manifest with each data file inlined
under `files` by its relative name, and no fixtures (ADR-0052 section
2). On Settings' Signals page, **Import a plugin** reads it (a
phone's `content://` file too) and keeps a copy named by its SHA-256
in the app's own folder, beside the signal files' copies, which is
loaded at each start. Plugins are the app's, not one vehicle's: every
variant of every plugin is a profile, so a URI naming one connects.
With a vehicle picked, the variants a plugin's `match` covers for the
car's VIN (the connected car's, or one typed in the VIN lookup; the
VIN itself is never stored) show as buttons, and the one picked is
kept with the vehicle and offered under **What to read**. The VIN only
proposes: nothing is chosen for you. Importing an id already there
replaces it, and **Remove** deletes the copy; neither goes ahead
while a session decodes with the plugin.
