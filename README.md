# reticulum-lora-repeater

A tiny, purpose-built [Reticulum](https://reticulum.network/) LoRa
transport repeater firmware for low-power nRF52 boards. Flash it from
your browser, configure over USB or Bluetooth, and your node is on the air.

**Web flasher:** https://thatSFguy.github.io/reticulum-lora-repeater/

**Target user:** already running [Meshtastic](https://meshtastic.org/)
or [MeshCore](https://github.com/ripplebiz/MeshCore) on an
nRF52840 + SX1262 board, curious about Reticulum, and wants to try
running a dedicated repeater without a toolchain install.

## Quick start

1. Visit **https://thatSFguy.github.io/reticulum-lora-repeater/**
2. Pick your **board** and **version**, click **Select firmware**
3. **Double-tap the reset button** on the board to enter bootloader mode
4. Click **Flash** and pick the new port that appears
5. After flashing, click **Connect USB** or **Connect BLE** to open the console
6. Edit your config (frequency, display name, TX power, location, etc.) and click **Commit & Reboot**

The node boots, starts relaying Reticulum packets over LoRa, and
announces itself on the mesh.

### Deploying multiple nodes

Configure one node, click **Export config** to save an `rlr-config.json`
file. For each additional node: flash, connect, click **Import config**,
change the display name, commit. Under 2 minutes per node after the first.

## What it is

- **Transport repeater.** Relays Reticulum DATA packets, announces, and
  delivery proofs over LoRa. Supports RNode-compatible split-packet
  reassembly and transmission for messages up to 508 bytes.
- **Runtime-configurable.** All settings — frequency, bandwidth, SF,
  CR, TX power, display name, battery calibration, location, Bluetooth —
  live in internal flash, not build flags. Edit via the web console,
  BLE, or serial terminal.
- **Web-flashable.** Visit the hosted page, plug in USB, click Flash.
  No PlatformIO, no toolchain, no `rnodeconf`. Under 5 minutes from
  bare board to on-air node.
- **Bluetooth Low Energy (BLE) configurable.** Connect from your phone
  via Web Bluetooth in Chrome — no USB cable needed. Custom GATT service
  for reliable config read/write, optional PIN pairing for security.
- **Observable.** Announces on `lxmf.delivery` so MeshChat / Sideband
  show it by name. Periodic telemetry announces report battery voltage,
  uptime, heap, packet counters, and GPS location.
- **Location-aware.** Configure latitude, longitude, and altitude (MSL)
  — included in telemetry announces. Use the "Use my location" button
  in the webapp to auto-populate from your phone's GPS.

## What it isn't

- **Not a KISS RNode replacement.** If you need a USB-connected RNode
  for Sideband on your laptop, use the upstream `RNode_Firmware`.
- **Not a LoRa chat client.** There's no user interface on the device.
- **Not a bridge to Meshtastic/MeshCore.** Reticulum is a different
  protocol at the LoRa framing level.

## Supported boards

| Board | Radio | Status |
|---|---|---|
| **Faketec** (Nice!Nano clone + Ebyte E22-900M30S) | SX1262 + ext PA | Bench-validated |
| **RAK4631** (WisBlock Core) | Integrated SX1262 | Shipping in releases |
| **XIAO nRF52840 Kit** (Seeed XIAO + Wio-SX1262 daughter) | SX1262 | Shipping — flash via UF2 drag-and-drop |
| **Heltec Mesh Node T114** | Integrated SX1262 | Shipping in releases |
| **RAK3401 1-Watt** (WisBlock + 1W PA) | SX1262 + 1W PA | Shipping in releases |

All five boards share the same firmware source — each board is just
one header file in `include/board/`. Adding a new nRF52840 + SX1262
board is a ~100-line header + one env block in `platformio.ini`.
See `docs/ADDING_A_BOARD.md`.

## Features

### Transport repeating

The firmware operates as a full Reticulum transport node:

- **DATA forwarding** — receives messages destined for non-local nodes and
  rebroadcasts them on all interfaces
- **ANNOUNCE rebroadcasting** — forwards announces from other nodes so
  the mesh learns paths through the repeater
- **PROOF forwarding** — delivery proofs are forwarded so senders get
  confirmation their message was received
- **RNode split-packet support** — messages larger than 254 bytes are
  automatically split into two LoRa frames on TX and reassembled on RX,
  compatible with standard RNode firmware framing
- **Automatic radio recovery** — if the SX1262 becomes unresponsive
  after flash I/O, the firmware hardware-resets and reconfigures it

### BLE wireless configuration

- **Custom GATT service** with three characteristics:
  - CONFIG (read/write) — pipe-delimited config string, atomic reads/writes
  - COMMIT (write) — write `0x01` to persist config and reboot
  - COMMAND (write) — text commands (ANNOUNCE, STATUS, DFU, REBOOT)
- **Nordic UART Service (NUS)** for real-time log streaming
- **PIN pairing** — optional 6-digit static passkey with MITM protection
- **LoRa paused during BLE** — radio activity is suspended while a BLE
  device is connected to prevent SoftDevice supervision timeouts
- **Web Bluetooth** — connect from Chrome on Android, macOS, Linux, or ChromeOS

### Configuration

All settings persist across reboots in internal flash (Config schema v2):

| Field | Range | Description |
|---|---|---|
| `display_name` | 1-31 chars | Node name (shown in Sideband/MeshChat) |
| `freq_hz` | 100-1100 MHz | LoRa frequency |
| `bw_hz` | 7.8-500 kHz | LoRa bandwidth |
| `sf` | 7-12 | Spreading factor |
| `cr` | 5-8 | Coding rate (4/5 to 4/8) |
| `txp_dbm` | -9 to +22 dBm | TX power at SX1262 core |
| `batt_mult` | 0.01-10.0 | ADC-to-mV calibration multiplier |
| `tele_interval_ms` | any | Telemetry announce interval |
| `lxmf_interval_ms` | any | LXMF presence announce interval |
| `telemetry` | on/off | Enable battery/health announces |
| `lxmf` | on/off | Enable LXMF presence announces |
| `heartbeat` | on/off | Enable heartbeat LED |
| `bt_enabled` | on/off | Enable BLE advertising |
| `bt_pin` | 0-999999 | BLE pairing PIN (0 = no PIN) |
| `latitude` | -90 to 90 | Node latitude (degrees) |
| `longitude` | -180 to 180 | Node longitude (degrees) |
| `altitude` | -100000 to 100000 | Altitude in meters MSL |

Config v1 records are automatically migrated to v2 on first boot.

### Web flasher / configurator

The hosted web app at https://thatSFguy.github.io/reticulum-lora-repeater/ provides:

- **Flash firmware** directly from the browser via Web Serial DFU
- **Connect USB** or **Connect BLE** to configure the node
- **Configuration form** with validation and human-friendly units
- **Battery calibration** — enter a multimeter voltage, firmware computes the multiplier
- **Config export/import** — save/load `rlr-config.json` for fleet provisioning
- **"Use my location" button** — auto-populate lat/lon/altitude from browser GPS
- **Status** and **Announce** buttons for on-demand diagnostics
- **DFU mode** entry via serial command
- **Real-time log streaming** via NUS (BLE) or serial (USB)

## Serial console commands

Connect via the web console or any serial terminal at 115200 8N1:

```
PING                       - liveness check
VERSION                    - firmware version
STATUS                     - runtime status (uptime, radio, packets, battery)
HELP                       - list all commands
REBOOT                     - NVIC system reset
DFU                        - reboot into DFU bootloader
ANNOUNCE                   - force LXMF + telemetry announce now
CONFIG GET                 - print staged config (key=value lines)
CONFIG GETP                - print staged config (pipe-delimited single line)
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
pio test -e native          # run unit tests (host-side, Unity framework)
```

Available build environments: `Faketec`, `RAK4631`, `XIAO_nRF52840`,
`Heltec_T114`, `RAK3401`, `native` (tests only).

## CI / Releases

Every tagged version (`v*`) triggers a GitHub Actions workflow that:
1. Builds firmware for all five boards in parallel
2. Creates a GitHub Release with `.zip`, `.hex`, and `.uf2` assets per board
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
- **Meshtastic** and **MeshCore** projects for nRF52840 variant pin
  maps and TCXO references during board bring-up

## License

MIT. See `LICENSE` for details and upstream attribution notices.
