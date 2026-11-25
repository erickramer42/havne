# Plugins

A plugin's data tier is a package of signal files
([docs/signal-files.md](../docs/signal-files.md)) with one manifest
that names the plugin, the cars it's for, the ECUs its files read and
its variants, each an ordered list of its files merged by signal key
([ADR-0052](../docs/adr/0052-plugin-framework.md)). Core loads a
package from its folder or packed into one file, and each variant
registers as the profile `plugin.<id>.<variant>`, which a URI's
`?profile=` names.

This folder holds the examples, `example-sim/` and `example-car/`.

`example-sim/` has made-up DIDs on two
modules (the engine at `7E8`, a transmission at `7E9`) that the
simulated ECU answers from the plugin's own definitions, with no car.
Its `base` variant extends `obd2` with five signals (one an
enumeration, one computed); `my2019` merges a second file on top,
which replaces two of them and adds one. `fixtures/values.json`
lists the value each signal should read back. A real make's plugin
stays with its author, never here.

`example-car/` is a whole car, made up: 18 modules (the engine ECU on its OBD pair, the rest on made-up addresses) in one variant, `whole`, with one data file for each module. Each module answers a name and a reading or two that the simulated ECU gives from the plugin's own definitions, and every number has a gauge. `plugins check --on <vcan>` reads all 18 at once; `SimCarBusTest` (Linux, on `vcan`) and `CliElm327RunTest` (the ELM327 emulator, every desktop platform) read them in CI.

## In the app

The app takes a plugin in its packed form, one file named `<id>.havne-plugin.json`: the manifest with each data file inlined under `files` by its relative name, and no fixtures (ADR-0052 section 2). On Settings' Signals page, **Import...** reads it (a phone's `content://` file too) and keeps a copy named by its SHA-256 in the app's own folder, which is loaded at each start. Plugins are the app's, not one vehicle's: every variant of every plugin is a row of the Signals page and a profile, so an address naming one connects. With a vehicle picked, a press on a row picks that variant for the vehicle; the variants a plugin's `match` covers for the car's VIN (the connected car's, or one typed in the VIN lookup; the VIN itself is never stored) are listed first under "For this car:". The VIN only proposes: nothing is chosen for you. Importing an id already there replaces it (a plugin marked **Mine** is updated in place), and **Remove** deletes the copy; neither goes ahead while a session decodes with the plugin.

## Making a plugin with havne-cli

`havne-cli plugins wrap <signal-file> --out <pkg>` turns a signal file into a packed plugin of one variant (`--id` and `--variant` name them), `plugins put <pkg> <signal-file> --out <pkg>` adds the file as a variant or replaces one, and `plugins drop <pkg> --variant <id> --out <pkg>` removes one. Each writes a new file and raises the plugin's minor version; `put` says when an ECU of the file took another id. `signals import obdb --plugin` writes an OBDb conversion as a plugin, its variant named for the model year you type, with `--wmi` and `--years` for the VINs it matches. `plugins check` names each variant's license and says when it is share-alike.

A plugin Havne makes carries `lineage`: 32 hex digits, the start of the SHA-256 of the signal file it was first made from. Leave it out of a plugin you write; keep it when you edit one Havne made, so a later version still updates the earlier one.

## A plugin that talks to a module

A plugin whose code talks to a car's module (the conversation tier) names its publisher in the manifest, format 3: `publisher` with `name` (up to 127 bytes), `contact` (255), `license` (63) and `terms` (up to 64 KiB), all UTF-8 plain text with no control character but line feeds and tabs in the terms. The terms are shown whole, as you wrote them, before anyone consents; Havne's own lines come first and your terms change none of them. A plugin that reads signals only may name a publisher or not, and needs no consent.

Nothing of the plugin's module runs against a car until the person has given consent to that version, in the app's dialog or with `havne-cli plugins consent --give <fingerprint>` after `havne-cli plugins terms <package>` has printed the terms and the fingerprint. The fingerprint is the SHA-256 of the plugin's contents (the manifest less `files` and `fixtures`, then each file's path and hash), so a folder and its packed file have one, and a changed byte anywhere asks again.

Havne doesn't check signatures, and a manifest with a `signature` key is refused. A publisher who signs publishes a detached signature over the package file, or over the fingerprint, with a tool of their own; the user checks it before consenting. A signature shows the plugin is unchanged since someone with that key signed it, never that it is safe or correct.
