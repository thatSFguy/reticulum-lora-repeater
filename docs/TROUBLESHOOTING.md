# Troubleshooting

A living document of known failure modes and their fixes, distilled
from the Faketec bring-up work that preceded this project.

## The eleven-bug boot cascade

When bringing up a new nRF52 + SX1262 board for the first time, these
are the failure modes you'll hit. They cascade — each one hides the
next — so work down the list systematically if `radio_online=false`
or `pin: 0 forever`:

1. **Wrong pin numbers.** Verify against a known-working reference
   variant: Meshtastic DIY `nrf52840/diy/*` or MeshCore `variants/*`.
2. **MOSI/LED pin collision.** `P0.15` vs `P1.15` look identical in
   a handwritten note. Always be explicit about the port prefix.
3. **`upload_protocol = jlink` default.** Most nRF52 PlatformIO board
   definitions default to J-Link, which fails with an opaque DLL
   error on any machine without J-Link hardware. **Always set
   `upload_protocol = nrfutil`** in your env.
4. **`post_upload` rnodeconf failures.** Upstream RNode firmware's
   `extra_script.py` runs provisioning after every `-t upload`. This
   project doesn't use that script; if you copy code from the sibling
   project, strip the post_upload hooks.
5. **Heap pool too large.** `RNS_HEAP_POOL_BUFFER_SIZE` > ~75% of the
   nRF52840's SRAM causes a silent hard fault before USB CDC
   enumerates. Start at 64 KB, raise to 96 KB only after confirming
   boot.
6. **`op_mode` default wrong for transport-only.** If the RNS init
   block reads `op_mode` before the BAKED config is applied, it
   comes up in `MODE_HOST` and transport is disabled. This project
   defaults to `MODE_TNC` at declaration time.
7. **`lora_*` globals zero at RNS init time.** Same root cause as #6:
   the RNS init reads config fields before they're populated. Seed
   them at declaration time in `Config.h`, or restructure setup()
   ordering.
8. **TCXO voltage wrong.** The Ebyte E22 family uses a **1.8 V**
   TCXO. Many reference drivers default to 3.3 V (because RAK4631
   does). Driving DIO3 at 3.3 V silently ruins the RF reference on
   both TX and RX. Set `RADIO_TCXO_VOLTAGE_MV` in the board header.
9. **SPI pins wrong.** Piggy-backing on the `pca10056` (nRF52840 DK)
   variant gives you default `PIN_SPI_MOSI=45, SCK=47, MISO=46` that
   collide with the Faketec's CS wiring at P1.13. **Always call
   `SPI.setPins(miso, sck, mosi)` before `SPI.begin()`** on boards
   that don't match the default variant. Set
   `RADIO_SPI_OVERRIDE_PINS = 1` in the board header.
10. **`device_init()` fails on unprovisioned EEPROM.** Upstream
    RNode firmware's `device_init()` returns
    `device_init_done && fw_signature_validated`, and the signature
    is only valid after rnodeconf-style EEPROM provisioning. This
    project bypasses that check entirely.
11. **`PIN_VEXT_EN` not asserted at boot.** Some boards (Faketec
    included) gate the radio 3V3 rail behind a GPIO. The pca10056
    variant's `initVariant()` actually drives `PIN_LED1 = P0.13` LOW
    at boot — which happens to be Faketec's VEXT_EN pin, so the
    radio is *actively de-powered* before setup() runs. Re-assert
    HIGH in setup() with a 10 ms settle delay.

Each bug is invisible until the previous one is fixed. The correct
debugging strategy is a non-blocking serial trace loop that polls
the SX1262's IRQ status register outside of any critical section
and prints on every state change. See the sibling project's
`[rxtrace]` diagnostic in `refactor.md` §11 for the exact pattern.

## Build cache quirks on Windows

- **SCons permission errors on network drives.** Symlinks/OneDrive
  folders sometimes trip SCons' `.sconsign` database. Fixes: add
  `.pio/` to Windows Defender exclusions, or set
  `build_dir = C:\platformio_builds\reticulum-lora-repeater` in
  `platformio.ini` to move the build tree onto a local disk while
  keeping source on the network drive.

- **"New code isn't in the binary."** If you add logging and the
  output doesn't appear, check BSS size in the heap stats dump. If
  BSS hasn't changed, the file wasn't recompiled. Always prefer
  `pio run -e <env> -t fullclean && pio run -e <env>` over an
  incremental build when debugging.

- **`pio device monitor` auto-port discovery crashes** on machines
  where the ststm32 platform is installed with a broken `rak4630`
  board definition. Workaround: always pass `--port COMxx`
  explicitly. Long-term fix: `pio pkg uninstall -g -p ststm32` if
  you don't need it.

## Hardware-specific surprises

### Nice!Nano clones

- **Battery divider ratios vary.** Some clones wire `P0.31` to the
  battery through a 2:1 divider; some don't; some have a different
  ratio entirely. Always calibrate `batt_mult` against a multimeter.
  Expect off-by-2× reports as the telltale signature of a wrong
  default.
- **LEDs vary.** Some clones ship with one LED, some with two (red +
  blue), with different polarities, and sometimes wired to different
  pins than the reference Nice!Nano. If your LED is solid-on after
  boot, check polarity first.

### Ebyte E22-900M30S

- **TCXO is 1.8 V** (not 3.3 V). See bug #8 above.
- **DIO2 drives TX antenna switching internally** — set
  `RADIO_DIO2_AS_RF_SWITCH = 1`.
- **RXEN must be driven HIGH in RX mode** — the sx126x driver handles
  this if you pass `PIN_LORA_RXEN` via `setPins()`.
- **External PA adds ~8 dB.** Firmware configures the SX1262 core at
  22 dBm max; antenna output is ~30 dBm / 1 W.

### Adafruit nRF52 bootloader

- **Only accepts `.uf2` (drag-drop) or `.zip` (serial DFU) files.**
  Dropping a `.bin` on the UF2 drive does nothing. Use
  `upload_protocol = nrfutil` + `pio run -t upload` which produces
  the `.zip` automatically.
- **Double-tap reset** forces the bootloader into DFU mode regardless
  of what the app did. Useful when the app has crashed and can't
  honor the 1200-baud touch reset.
- **Signature mismatch → stuck in bootloader with blinking LED.** If
  the app's firmware signature in the bootloader settings page at
  `0xFF000` doesn't match the flashed binary, the bootloader refuses
  to start the app. The `.zip` upload path rewrites this page
  atomically; drag-dropping a UF2 built from a plain `.bin` does
  not. Prefer `-t upload` over manual UF2 drag-drop.

### Reticulum air interop

- **Reticulum is not Meshtastic or MeshCore at the wire level.** They
  share the LoRa physical layer (freq/BW/SF/CR) but use completely
  different sync words, framing, encryption, and routing. A
  Reticulum node cannot hear or be heard by a Meshtastic/MeshCore
  node even with perfectly matched modulation. All peers of this
  firmware must also be running Reticulum-family firmware.
- **SX127x and SX126x sync words are encoded differently.** A
  matched pair is SX127x `0x12` ↔ SX126x `0x1424`
  (Semtech AN1200.48). Our driver hard-codes the SX126x side to
  `0x1424`.

## When all else fails

Capture the first 60 seconds of serial output starting from a clean
boot, post it as a GitHub issue with the board, firmware version,
and exact platformio.ini env snippet. The path store stats + heap
pool stats + any `[rxtrace]`-style diagnostic lines are the most
important debugging data.
