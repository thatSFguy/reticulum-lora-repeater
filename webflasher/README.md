# Webflasher

A static site for flashing + provisioning reticulum-lora-repeater
over Web Serial, deployable as-is on GitHub Pages. **Not implemented
yet** — Phase 6 work.

## Planned architecture

```
webflasher/
├── index.html              # single page, minimal CSS
├── app.js                  # UI logic + state machine
├── lib/
│   ├── dfu.js              # JS-native Adafruit DFU transport
│   │                       # (ported from liamcottle/rnode-flasher
│   │                       #  with attribution, provisioning step
│   │                       #  stripped)
│   └── console.js          # line-oriented serial console client
│                           # implementing docs/SERIAL_PROTOCOL.md
├── ui/
│   ├── wizard.js           # step machine: connect → flash → configure → verify
│   ├── region.js           # region → frequency range mapping
│   └── schema.js           # Config field list + per-field validators
└── firmware/
    ├── manifest.json       # {board: [{version, filename, sha256}], ...}
    └── *.zip               # populated by CI from GitHub Releases
```

## User flow (design)

1. **Landing page** with a single prominent "Flash your repeater"
   button. Subtitle: "Coming from Meshtastic or MeshCore? This
   firmware runs on the same hardware and gives you a Reticulum
   transport repeater in about 5 minutes."
2. **Board select** — dropdown of supported boards. v0.1 is Faketec
   only; v0.2 adds RAK4631 + XIAO.
3. **Region select** — mandatory before anything else. Drives the
   allowed frequency ranges in later steps. Can't be changed without
   re-flashing.
4. **Connect** — browser prompt for the bootloader COM port. App.js
   sends the 1200-baud touch to auto-enter DFU mode if the board is
   currently running an Adafruit-bootloader app. Falls back to
   manual instructions ("unplug, hold reset, plug in") if not.
5. **Flash** — fetches the matching `.zip` from `firmware/` and
   pushes it via JS-native DFU. Progress bar.
6. **Reconnect** — the board reboots into the newly-flashed firmware
   and re-enumerates on a new COM port. App.js reconnects
   automatically (user re-selects the port if needed).
7. **Configure form** — auto-generated from `ui/schema.js`. Every
   field from `docs/CONFIG_FORMAT.md` has an input with the right
   type, range constraints, and defaults. Frequency presets per
   region. Display name required.
8. **Review** — shows a summary: "About to apply: freq=904.375 MHz,
   BW=250 kHz, SF=10, ..., name='Solar Site A'. Proceed?"
9. **Apply** — sends the `CONFIG SET` sequence, then `CONFIG
   COMMIT`, then waits ~3 s and reconnects. Verifies via `STATUS`.
10. **Calibrate battery** (optional next step) — shows current
    `batt_mv`, asks for multimeter reading, computes correction,
    sends one more `CONFIG SET batt_mult` + `CONFIG COMMIT`.
11. **Done** — green check, current `STATUS` readout, link to
    `scripts/telemetry_receiver.py` for remote monitoring.

## Why a fresh build instead of forking rnode-flasher

liamcottle/rnode-flasher is an excellent reference and we will
reuse its JS-native DFU transport almost verbatim (with attribution
in `lib/dfu.js`). But the UI and the post-flash provisioning it does
are tightly coupled to upstream RNode EEPROM provisioning
(firmware hash stamping, product/model/hwrev codes), which is
exactly the thing this project deliberately skips. Rather than
fork-and-strip, we're building the UI fresh around the
docs/SERIAL_PROTOCOL.md command set.

## Dependencies

- None. Vanilla HTML + JS + CSS. The target user should be able to
  load a static page and have it work; a build step is a regression.
- Web Serial API (Chrome / Edge / Opera desktop). Firefox doesn't
  implement Web Serial and is not supported.

## Deployment

GitHub Pages from the `gh-pages` branch or a `/docs` folder. CI
updates `webflasher/firmware/*.zip` on every release tag. A single
URL — `thatSFguy.github.io/reticulum-lora-repeater/` — is the only
entry point an end user ever needs to know about.
