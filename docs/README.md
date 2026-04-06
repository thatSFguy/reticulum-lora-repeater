# Reticulum LoRa Repeater — Web Flasher & Configurator

Live: **https://thatSFguy.github.io/reticulum-lora-repeater/**

Flash firmware and configure your Reticulum LoRa Repeater node
directly from the browser. No PlatformIO, no toolchain, no
`rnodeconf`. Works in any Chromium-based browser on desktop.

## Supported boards

| Board | Radio | Notes |
|---|---|---|
| **Faketec** (Nice!Nano + Ebyte E22-900M30S) | SX1262 + ext PA | Bench-validated |
| **RAK4631** (WisBlock Core) | Integrated SX1262 | |
| **XIAO nRF52840 Kit** (Seeed XIAO + Wio-SX1262) | SX1262 | |
| **Heltec Mesh Node T114** | Integrated SX1262 | Has TFT display (unused by firmware) |
| **RAK3401 1-Watt** (WisBlock + 1W PA) | SX1262 + 1W PA | Higher default TX power |

The board dropdown is populated automatically from the CI release
pipeline. New boards appear as soon as they're added to the build
matrix.

## How to use

### 1. Flash a new board

1. Pick a **version** and **board** from the dropdowns, click **Select firmware**
2. **Double-tap the reset button** on the board (within ~500 ms) to enter bootloader mode
3. Click **Flash** and pick the new bootloader port in the picker
4. Wait for the progress bar (~15 seconds)

### 2. Configure after flashing

1. Click **Connect** and pick the application port (the normal USB port, not the bootloader)
2. The **Status** panel shows uptime, radio state, packet counters, and battery
3. Edit settings in the **Configuration** panel:
   - **Frequency** in MHz (e.g. `904.375`)
   - **Bandwidth** from dropdown (7.8 kHz to 500 kHz)
   - **SF** (7–12), **CR** (4/5–4/8), **TX power** (-9 to +22 dBm)
   - **Telemetry** and **LXMF** intervals in minutes
   - **Display name** — what shows up in MeshChat/Sideband
4. Click **Commit & Reboot** — saves to flash and restarts

### 3. Calibrate battery

1. Measure the actual voltage with a multimeter
2. Enter the value in mV in the **Battery calibration** panel
3. Click **Calibrate** → firmware computes the multiplier
4. Click **Commit & Reboot** to persist

### 4. Deploy multiple nodes

1. Configure the first node as above
2. Click **Export config** → downloads `rlr-config.json`
3. Flash the next node
4. Connect, click **Import config**, pick the JSON
5. Change **display name** to something unique for this node
6. Click **Commit & Reboot**
7. Repeat 3–6 for each node

The exported JSON is human-readable and editable:
```json
{
  "display_name": "Solar Repeater North",
  "freq_mhz": 904.375,
  "bw_hz": 250000,
  "sf": 10,
  "cr": 5,
  "txp_dbm": 22,
  "tele_interval_min": 180,
  "lxmf_interval_min": 30,
  "telemetry": true,
  "lxmf": true,
  "heartbeat": true
}
```

### 5. Live monitoring

When connected, the **Log** panel streams all firmware output in
real time — alive markers, per-packet radio diagnostics (RSSI, SNR,
packet type), and Reticulum debug output. No PlatformIO serial
monitor needed.

## Browser requirements

- **Chromium-based browser** (Chrome, Edge, Opera, Brave) on desktop.
  Firefox and Safari do not support the Web Serial API.
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
| `index.html`       | Single-page UI — flash, configure, calibrate, export/import |
| `console.js`       | Web Serial client + release picker + config management     |
| `dfu.js`           | Nordic legacy serial DFU client (SLIP + CRC16 + HCI)      |
| `DFU_PROTOCOL.md`  | Byte-by-byte DFU wire format reference                     |
| `firmware/`        | Per-release firmware binaries (same-origin, no CORS)       |
| `.nojekyll`        | Disables Jekyll so static files serve as-is                |

## Serial protocol reference

See `SERIAL_PROTOCOL.md` and `src/SerialConsole.cpp` in the repo.
Every response ends with `OK` or `ERR: <reason>`.
