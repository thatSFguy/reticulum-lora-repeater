# Adding a new board

This firmware is designed so that porting to a new nRF52 board is
**one header file and one env block**. No edits in `src/`, no edits
in the SX1262 driver, no scattered `#ifdef` chains.

## Prerequisites

- An nRF52840 (or nRF52833) board with the Adafruit nRF52 bootloader
  flashed. If it's not already running an Adafruit-bootloader firmware
  (Meshtastic, MeshCore, upstream RNode, etc.), you'll need to flash
  the bootloader first — out of scope for this doc.
- An SX1262 or SX1268 radio module wired to the MCU via SPI. Other
  radios will land in future phases.
- The board's schematic / pinout reference. Cross-check against at
  least one other firmware that has this board working — MeshCore's
  `variants/` directory and Meshtastic's `variants/nrf52840/` are
  excellent authoritative sources.

## Steps

### 1. Create `include/board/<your_board>.h`

Copy `include/board/Faketec.h` as a starting template. Every constant
in that file must be reviewed and updated for your hardware. Key
items that commonly differ:

- `BOARD_<NAME>` ID, `PRODUCT_<NAME>`, `MODEL_<NAME>`
- `HAS_TCXO` — does your radio module have an external TCXO? Most E22
  and RAK modules do; some cheaper SX1262 breakouts don't.
- `RADIO_TCXO_VOLTAGE_MV` — **critical**. E22 uses `1800`, RAK4631
  uses `3300`. Getting this wrong silently deafens the radio.
- `RADIO_SPI_OVERRIDE_PINS` — set to `1` if your PlatformIO `board = `
  value's variant has default `PIN_SPI_*` that don't match your
  wiring. Most custom wirings need this. See the Faketec case for
  why.
- `PIN_LORA_*` — from your schematic
- `PIN_VEXT_EN` / `HAS_VEXT_RAIL` — some boards gate the radio power
  rail behind a GPIO that the MCU has to drive HIGH at boot
- `PIN_LED`, `LED_ACTIVE_HIGH` — LED polarity varies; check with a
  multimeter if you're unsure
- `PIN_BATTERY` — the ADC input for battery voltage monitoring
- `DEFAULT_CONFIG_BATT_MULT` — calibrated against a multimeter. Leave
  at `1.0f` initially and tune post-flash.

### 2. Add an `[env:<your_board>]` block to `platformio.ini`

Copy `[env:Faketec]` as a template. The critical changes are:

- `board = <pio-board-name>` — whichever PlatformIO board definition
  matches your hardware
- `-DBOARD_MODEL=BOARD_<YOUR_NAME>`
- `-include include/board/<your_board>.h`
- `upload_protocol = nrfutil` — always, for Adafruit-bootloader boards

### 3. Build and flash

```bash
pio run -e <your_board>
pio run -e <your_board> -t upload --upload-port COMxx
```

If the build fails, the compiler messages will usually point at a
missing or misnamed constant in your board header. If the build
succeeds but the radio stays offline at boot, work through
`docs/TROUBLESHOOTING.md` — it catalogs the eleven distinct failure
modes encountered during the original Faketec bring-up.

### 4. Calibrate the battery multiplier

Connect the board to a known-voltage source (USB 5V rail, or a
battery + multimeter). Read `bat=...` in the serial telemetry output,
then:

```
new_mult = current_mult * (measured_mv / reported_mv)
```

Set via the serial console:

```
CONFIG SET batt_mult 1.284
CONFIG COMMIT
```

Or bake the corrected default into your board header's
`DEFAULT_CONFIG_BATT_MULT` so fresh units ship with the right value.

### 5. Submit a PR

Pin your board header + the updated `platformio.ini` in a single
commit, tagged with the board model in the message. A photo of the
working board + a log excerpt showing `paths: 1+` is the ideal
acceptance test artifact.

## What NOT to edit

If you find yourself needing to touch any of these files to get a
new board working, something has gone wrong — stop and check whether
the board header's `HAS_*`, `PIN_*`, `RADIO_*` macros can express
what you need instead:

- `src/main.cpp` — generic, board-agnostic
- `src/Radio.cpp` — reads board constants, doesn't know board names
- `src/drivers/sx126x.cpp` — reads `RADIO_TCXO_VOLTAGE_MV` and
  `RADIO_SPI_OVERRIDE_PINS` macros, no board name `#if` chains
- `src/Transport.cpp`, `src/Telemetry.cpp`, `src/LxmfPresence.cpp` —
  all board-agnostic
- `src/Led.cpp`, `src/Battery.cpp` — read board macros only

**The point of the whole architecture is that board support is
additive, never subtractive.** If you're deleting or rewriting
shared code to get a board working, file an issue — we need to
grow the macro vocabulary instead.
