# Reticulum LoRa Repeater — Web Configurator

Live: **https://thatSFguy.github.io/reticulum-lora-repeater/**

A single-page Web Serial app that talks to the firmware's built-in
serial provisioning console (`src/SerialConsole.cpp`). It lets a user
connect a flashed node over USB, read its runtime status, edit the
persistent Config, and calibrate the battery ADC — all without a
PlatformIO install or a local serial monitor.

## Supported boards (v0.1)

| Board                     | Radio module          | Status                               |
|---------------------------|-----------------------|--------------------------------------|
| **Faketec** (Nice!Nano + Ebyte E22-900M30S) | SX1262 + ext PA | Bench-validated end-to-end |
| **RAK4631** (WisBlock Core)                 | Integrated SX1262  | Builds green, hardware validation pending (see `docs/RATCHET_PROTOCOL.md` / `include/board/rak4631.h`) |

More boards in the roadmap (XIAO nRF52840, etc). The webflasher's
version + board dropdowns are populated from the per-release manifest,
so any board the CI matrix builds automatically shows up here.

## What works today (Phase 6a + 6b)

- **Flash firmware over Web Serial** directly to the Adafruit nRF52
  bootloader (SLIP-framed Nordic legacy DFU). Pick a version from the
  release dropdown — no toolchain install needed. Wire format is
  documented byte-for-byte in `DFU_PROTOCOL.md`; the implementation
  lives in `dfu.js`.
- Read live `STATUS` (uptime, radio state, packet counters, battery
  raw/scaled readings, display name).
- Read the full persistent `CONFIG GET` into an editable form.
- One-step battery calibration: enter a multimeter reading in mV,
  click Calibrate, the firmware computes `batt_mult = measured / raw`
  and stages it.
- Stage config edits in the browser, push them to the device with a
  single Commit & Reboot click.
- Revert, Reset to defaults, Reboot — all of the console's commands
  are wired to buttons.

## Entering bootloader mode

The Adafruit nRF52 bootloader exposes a different USB CDC device than
the application firmware. **Double-tap the reset button within ~500 ms**
to put the board in bootloader mode — it will re-enumerate with a new
VID/PID pair and appear as a different COM port. Pick that port in
the Web Serial picker when you click Flash.

## Browser requirements

- **Chromium-based browser** (Chrome, Edge, Opera, Brave) on desktop.
  Firefox and Safari do not implement the Web Serial API.
- **Secure context** — the page must be served over `https://` or
  from `http://localhost`. GitHub Pages provides HTTPS automatically.

## Local development

```
cd docs
python -m http.server 8000
# open http://localhost:8000/
```

Web Serial works over `http://localhost` so you can iterate on the UI
without deploying to Pages.

## Files

| File               | Purpose                                                      |
|--------------------|--------------------------------------------------------------|
| `index.html`       | Single-page UI — structure, styling, form fields             |
| `console.js`       | `RLRConsole` Web Serial client + DOM glue                    |
| `dfu.js`           | Nordic legacy serial DFU client (SLIP + CRC16 + HCI)          |
| `DFU_PROTOCOL.md`  | Byte-by-byte wire format reference `dfu.js` implements       |
| `.nojekyll`        | Disables Jekyll on GitHub Pages so assets serve unmodified   |

## Protocol reference

See `docs/SERIAL_PROTOCOL.md` and `src/SerialConsole.cpp` in this repo
for the authoritative command list. Every response ends with either a
line reading `OK` or `ERR: <reason>` — the web client reads until it
sees one of those.
