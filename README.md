# reticulum-lora-repeater

A tiny, purpose-built [Reticulum](https://reticulum.network/) LoRa
transport repeater firmware for low-power nRF52 boards. Flash it from
your browser, fill out a form, and your solar node is on the air.

**Target user:** already running [Meshtastic](https://meshtastic.org/)
or [MeshCore](https://github.com/ripplebiz/MeshCore) on a Nice!Nano-style
nRF52840 + SX1262 board, curious about Reticulum, and wants to try
running a dedicated repeater without a toolchain install or
`rnodeconf` configuration dance.

## Status

**Pre-alpha.** This repo was scaffolded from a working sibling project
([microReticulum_Faketec_Repeater](../microReticulum_Faketec_Repeater))
that has a real on-air transport node running in production. The
structure here is new; the radio code, LXMF encoding, battery
telemetry, heap tuning, and 11-bug-deep bring-up knowledge are all
ported from the sibling project. See `docs/TROUBLESHOOTING.md` for the
collected tribal knowledge.

## What it is

- **Transport-only.** The firmware does one job: sit there and relay
  Reticulum packets over LoRa. No KISS host mode, no display, no
  BLE (yet), no configuration file, no PC toolchain.
- **Runtime-configurable.** All settings — frequency, bandwidth, SF,
  CR, TX power, display name, battery calibration — live in internal
  flash, not build flags. Change a setting, no reflash required.
- **Web-flashable.** Visit the hosted page, plug your board in via USB,
  click Flash, fill out a form, click Apply. Under 5 minutes from bare
  board to on-air node.
- **Small.** Target budget: < 2000 lines of application source, fits
  comfortably in an nRF52840's flash with room for Reticulum's stack
  and microStore persistence.
- **Observable in LXMF clients.** Announces itself under an LXMF
  `lxmf.delivery` aspect so MeshChat / Sideband / NomadNet show it
  in their network visualizers with a human-readable name.
- **Self-reporting.** Periodically announces battery voltage + health
  (free SRAM, RSSI, uptime, packet counters) on a custom telemetry
  aspect. Run the companion Python receiver on any Reticulum-aware
  host to log remote solar nodes without visiting them.

## What it isn't

- **Not a KISS RNode replacement.** If you need a USB-connected RNode
  for a Sideband/NomadNet endpoint on your laptop, keep using the
  upstream `RNode_Firmware`. This firmware doesn't speak the KISS
  host protocol at all.
- **Not a LoRa chat client.** There's no user interface on the device.
  The display name is just a label for the visualizer.
- **Not a bridge to Meshtastic or MeshCore meshes.** Reticulum is a
  different protocol at the LoRa air-framing level; it can't hear
  Meshtastic packets and vice versa. Your nodes will need to all run
  Reticulum-compatible firmware (this one, upstream RNode, etc.) to
  form a single mesh.

## Supported boards

| Board | Radio | Status |
|---|---|---|
| Faketec (Nice!Nano clone + Ebyte E22-900M30S SX1262) | SX1262 | ✅ v0.1 target |
| RAK4631 | SX1262 | 📋 stub header, v0.2 target |
| Seeed XIAO nRF52840 + E22 | SX1262 | 📋 stub header, v0.2 target |

Adding a new board is one header file in `include/board/` plus one
env block in `platformio.ini` — see `docs/ADDING_A_BOARD.md`.

## Get started

**With the webflasher (recommended, coming soon):** Visit
`<tbd>.github.io/reticulum-lora-repeater/` and follow the prompts.

**From source (developer workflow):**

```bash
git clone https://github.com/thatSFguy/reticulum-lora-repeater
cd reticulum-lora-repeater
pio run -e Faketec -t upload --upload-port COMxx
pio device monitor -e Faketec --port COMxx
```

Then paste this into the monitor to provision:

```
CONFIG SET freq_hz 904375000
CONFIG SET bw_hz 250000
CONFIG SET sf 10
CONFIG SET cr 5
CONFIG SET txp_dbm 22
CONFIG SET display_name "My Repeater"
CONFIG COMMIT
```

The node reboots, picks up the new config from flash, and starts
announcing on LoRa.

## Acknowledgements

This firmware would not exist without:

- **Mark Qvist** for [Reticulum](https://github.com/markqvist/Reticulum),
  [RNode_Firmware](https://github.com/markqvist/RNode_Firmware), and
  [LXMF](https://github.com/markqvist/LXMF). This project is downstream
  of all three.
- **Chad Attermann** for
  [microReticulum](https://github.com/attermann/microReticulum),
  [microStore](https://github.com/attermann/microStore), and
  [micropython-reticulum](https://github.com/attermann/micropython-reticulum).
  The C++ Reticulum stack, microStore persistence, and the LXMF
  announce wire-format reference all come from his work.
- **Liam Cottle** for
  [rnode-flasher](https://github.com/liamcottle/rnode-flasher) — the
  webflasher design is based on his JS-native DFU implementation.
- **The MeshCore project** for its
  [`nrf52840/diy/nrf52_promicro_diy_tcxo`](https://github.com/meshtastic/firmware/tree/master/variants/nrf52840/diy/nrf52_promicro_diy_tcxo)
  (Meshtastic) and `variants/promicro/` (MeshCore) references, which
  were the authoritative source for the Faketec pin map, TCXO voltage,
  and VEXT_EN init sequence during initial bring-up.

## License

MIT. See `LICENSE` for details and upstream attribution notices.
