# Reticulum LoRa Repeater — Web Flasher & Configurator

Live: **https://thatSFguy.github.io/reticulum-lora-repeater/**

Flash firmware and configure your Reticulum LoRa Repeater node
directly from the browser. No PlatformIO, no toolchain, no
`rnodeconf`. Works in any Chromium-based browser on desktop.

## How to use

### Flash a new board

1. Pick a **version** and **board** from the dropdowns, click **Select firmware**
2. **Double-tap the reset button** on the board (within ~500 ms) to enter bootloader mode — the device re-enumerates as a new USB serial port
3. Click **Flash** and pick the new bootloader port in the browser's picker
4. Wait for the progress bar to complete (~15 seconds)

### Configure after flashing

1. Click **Connect** (in the Configure panel) and pick the application port (the normal USB port, not the bootloader)
2. The **Status** panel shows uptime, radio state, packet counters, and battery readings
3. Edit settings in the **Configuration** panel (frequency, bandwidth, SF, CR, TX power, display name, feature flags)
4. Click **Commit & Reboot** — the node saves to flash and restarts with the new settings

### Calibrate battery

1. Measure the actual battery/USB rail voltage with a multimeter
2. Enter the measured value in millivolts in the **Battery calibration** panel
3. Click **Calibrate** — the firmware computes the multiplier automatically
4. Click **Commit & Reboot** to persist

## Supported boards

| Board | Radio | Status |
|---|---|---|
| **Faketec** (Nice!Nano + Ebyte E22-900M30S) | SX1262 + ext PA | Bench-validated |
| **RAK4631** (WisBlock Core) | Integrated SX1262 | Builds green, hardware validation pending |

The version + board dropdowns are populated from a firmware manifest
that the CI pipeline regenerates on every release. New boards
automatically appear when added to the CI matrix.

## Browser requirements

- **Chromium-based browser** (Chrome, Edge, Opera, Brave) on desktop.
  Firefox and Safari do not implement the Web Serial API.
- **Secure context** — HTTPS or localhost. GitHub Pages provides
  HTTPS automatically.

## Local development

```
cd docs
python -m http.server 8000
# open http://localhost:8000/
```

## Files

| File               | Purpose                                                    |
|--------------------|------------------------------------------------------------|
| `index.html`       | Single-page UI — flash panel, config form, battery cal     |
| `console.js`       | `RLRConsole` Web Serial client + release picker + UI glue  |
| `dfu.js`           | Nordic legacy serial DFU client (SLIP + CRC16 + HCI)      |
| `DFU_PROTOCOL.md`  | Byte-by-byte wire format reference for `dfu.js`           |
| `firmware/`        | Per-release firmware binaries served same-origin           |
| `.nojekyll`        | Disables Jekyll so static files serve as-is                |

## Serial protocol reference

See `SERIAL_PROTOCOL.md` and `src/SerialConsole.cpp` in the repo for
the full command list. Every response ends with `OK` or `ERR: <reason>`.
