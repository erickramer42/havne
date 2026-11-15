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

The app takes a plugin in its packed form, one file named `<id>.havne-plugin.json`: the manifest with each data file inlined under `files` by its relative name, and no fixtures (ADR-0052 section 2). On Settings' Signals page, **Import...** reads it (a phone's `content://` file too) and keeps a copy named by its SHA-256 in the app's own folder, which is loaded at each start. Plugins are the app's, not one vehicle's: every variant of every plugin is a row of the Signals page and a profile, so an address naming one connects. With a vehicle picked, a press on a row picks that variant for the vehicle; the variants a plugin's `match` covers for the car's VIN (the connected car's, or one typed in the VIN lookup; the VIN itself is never stored) are listed first under "For this car:". The VIN only proposes: nothing is chosen for you. Importing an id already there replaces it (a plugin marked **Mine** is updated in place), and **Remove** deletes the copy; neither goes ahead while a session decodes with the plugin.

## Making a plugin with havne-cli

`havne-cli plugins wrap <signal-file> --out <pkg>` turns a signal file into a packed plugin of one variant (`--id` and `--variant` name them), `plugins put <pkg> <signal-file> --out <pkg>` adds the file as a variant or replaces one, and `plugins drop <pkg> --variant <id> --out <pkg>` removes one. Each writes a new file and raises the plugin's minor version; `put` says when an ECU of the file took another id. `signals import obdb --plugin` writes an OBDb conversion as a plugin, its variant named for the model year you type, with `--wmi` and `--years` for the VINs it matches. `plugins check` names each variant's license and says when it is share-alike.

A plugin Havne makes carries `lineage`: 32 hex digits, the start of the SHA-256 of the signal file it was first made from. Leave it out of a plugin you write; keep it when you edit one Havne made, so a later version still updates the earlier one.
