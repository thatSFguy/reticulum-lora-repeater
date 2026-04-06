# reticulum-lora-repeater

A tiny, purpose-built [Reticulum](https://reticulum.network/) LoRa
transport repeater firmware for low-power nRF52 boards. Flash it from
your browser, configure over USB, and your node is on the air.

**Web flasher:** https://thatSFguy.github.io/reticulum-lora-repeater/

**Target user:** already running [Meshtastic](https://meshtastic.org/)
or [MeshCore](https://github.com/ripplebiz/MeshCore) on a Nice!Nano-style
nRF52840 + SX1262 board, curious about Reticulum, and wants to try
running a dedicated repeater without a toolchain install.

## Quick start

1. Visit **https://thatSFguy.github.io/reticulum-lora-repeater/**
2. Pick your board and version, click **Select firmware**
3. Double-tap the reset button on the board to enter bootloader mode
4. Click **Flash** and pick the new port that appears
5. After flashing, click **Connect** to open the serial console
6. Edit your config (frequency, display name, TX power, etc.) and click **Commit & Reboot**

The node boots, starts relaying Reticulum packets over LoRa, and
announces itself on the mesh.

## What it is

- **Transport-only.** The firmware does one job: relay Reticulum
  packets over LoRa. No KISS host mode, no display, no BLE.
- **Runtime-configurable.** All settings — frequency, bandwidth, SF,
  CR, TX power, display name, battery calibration — live in internal
  flash, not build flags. Edit via the web console or serial terminal.
- **Web-flashable.** Visit the hosted page, plug in USB, click Flash.
  No PlatformIO, no toolchain, no `rnodeconf`. Under 5 minutes from
  bare board to on-air node.
- **Observable.** Announces on `lxmf.delivery` so MeshChat / Sideband
  show it by name. Periodic telemetry announces report battery voltage,
  uptime, heap, and packet counters.
- **Small.** ~74% flash, ~9% RAM on nRF52840. Room for the full
  Reticulum stack + microStore persistence.

## What it isn't

- **Not a KISS RNode replacement.** If you need a USB-connected RNode
  for Sideband on your laptop, use the upstream `RNode_Firmware`.
- **Not a LoRa chat client.** There's no user interface on the device.
- **Not a bridge to Meshtastic/MeshCore.** Reticulum is a different
  protocol at the LoRa framing level.

## Supported boards

| Board | Radio | Status |
|---|---|---|
| **Faketec** (Nice!Nano clone + Ebyte E22-900M30S) | SX1262 + ext PA | Bench-validated, shipping in releases |
| **RAK4631** (WisBlock Core) | Integrated SX1262 | Builds green, shipping in releases, hardware validation pending |
| XIAO nRF52840 + SX1262 | SX1262 | Stub header, future target |

Adding a new board is one header file in `include/board/` plus one
env block in `platformio.ini` — see `docs/ADDING_A_BOARD.md`.

## Serial console commands

Connect via the web console or any serial terminal at 115200 8N1:

```
PING                       - liveness check
VERSION                    - firmware version
STATUS                     - runtime status (uptime, radio, packets, battery)
HELP                       - list all commands
REBOOT                     - NVIC system reset
ANNOUNCE                   - force LXMF + telemetry announce now
CONFIG GET                 - print staged config
CONFIG SET <key> <value>   - stage a field change
CONFIG RESET               - reseed staging from board defaults
CONFIG REVERT              - reseed staging from live config
CONFIG COMMIT              - persist staging + reboot
CALIBRATE BATTERY <mv>     - derive batt_mult from measured voltage
```

## From source (developer workflow)

```bash
git clone https://github.com/thatSFguy/reticulum-lora-repeater
cd reticulum-lora-repeater
pio run -e Faketec -t upload --upload-port COMxx
pio device monitor -e Faketec --port COMxx
```

## CI / Releases

Every tagged version (`v*`) triggers a GitHub Actions workflow that:
1. Builds firmware for every board in the matrix (Faketec, RAK4631)
2. Creates a GitHub Release with `.zip` and `.hex` assets per board
3. Publishes firmware to `docs/firmware/<tag>/` for the web flasher
4. Regenerates the firmware manifest so the web flasher auto-discovers new versions

## Acknowledgements

- **Mark Qvist** for [Reticulum](https://github.com/markqvist/Reticulum),
  [RNode_Firmware](https://github.com/markqvist/RNode_Firmware), and
  [LXMF](https://github.com/markqvist/LXMF)
- **Chad Attermann** for
  [microReticulum](https://github.com/attermann/microReticulum) and
  [microStore](https://github.com/attermann/microStore) — the C++
  Reticulum stack and persistence layer this firmware runs on
- **Liam Cottle** for
  [rnode-flasher](https://github.com/liamcottle/rnode-flasher) — the
  web flasher's DFU protocol implementation is based on his work
- **Meshtastic** and **MeshCore** projects for the nRF52840 ProMicro
  DIY variant pin maps and TCXO reference during initial bring-up

## License

MIT. See `LICENSE` for details and upstream attribution notices.
