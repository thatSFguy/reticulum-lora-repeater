# Adafruit nRF52 Serial DFU — Wire Format

Authoritative reference: `nordicsemi/dfu/dfu_transport_serial.py` and
`nordicsemi/dfu/util.py` in [adafruit/Adafruit_nRF52_nrfutil]
(https://github.com/adafruit/Adafruit_nRF52_nrfutil). This document
exists so the JavaScript implementation in `docs/dfu.js` has a frozen
spec to match byte-for-byte without re-parsing the Python source.

## Packet envelope

Every packet sent to or received from the bootloader is an **HCI
reliable packet** wrapped in SLIP framing:

```
[0xC0] [ SLIP-escape( preamble(4) || payload(N) || crc16(2) ) ] [0xC0]
```

- `0xC0` — SLIP frame delimiter (END). Present before AND after the body.
- Body length is not prefixed in the frame; the second `0xC0` ends it.
- CRC-16 covers **preamble + payload**, little-endian append (low byte first).

## SLIP escape (body only, never on the framing `0xC0`s)

| input byte | output bytes |
|------------|--------------|
| `0xC0`     | `0xDB 0xDC`  |
| `0xDB`     | `0xDB 0xDD`  |
| anything else | byte as-is |

Decode is the inverse. Any `0xDB` must be followed by `0xDC` or
`0xDD`; anything else is a protocol error.

## HCI preamble (4 bytes)

`HciPacket` increments a class-wide sequence counter `seq = (seq+1) % 8`
before each packet. The preamble encodes that seq, the next seq, a
type/length, and a one-byte header checksum.

```
byte 0:  bit 7    = reliable_packet        = 1
         bit 6    = data_integrity_present = 1
         bits 5-3 = (seq + 1) % 8          (next sequence number)
         bits 2-0 = seq                    (this packet's sequence)

byte 1:  bits 7-4 = pkt_len & 0x00F        (length low nibble)
         bits 3-0 = pkt_type               = 14  (HCI reliable data)

byte 2:  bits 7-0 = (pkt_len & 0xFF0) >> 4 (length middle byte)

byte 3:  header checksum = (~(byte0 + byte1 + byte2) + 1) & 0xFF
         (two's complement of the sum, wrapped to 8 bits)
```

`pkt_len` is the length of the **payload** in bytes (before CRC, before
SLIP encoding). Max payload fits 12 bits (4095), well above the 512 B
data packet cap used here.

## CRC-16 (CCITT-FALSE)

- **Polynomial**: 0x1021
- **Init**: 0xFFFF
- **Reflection**: none (neither input nor output)
- **XOR-out**: 0x0000

Reference byte-at-a-time implementation (matches `crc16.py`):

```c
uint16_t crc = 0xFFFF;
for each input byte b:
    crc = ((crc >> 8) & 0x00FF) | ((crc << 8) & 0xFF00);
    crc ^= b;
    crc ^= (crc & 0x00FF) >> 4;
    crc ^= (crc << 8) << 4;
    crc ^= ((crc & 0x00FF) << 4) << 1;
```

All intermediate values truncated to 16 bits.

## Payload opcodes

All payload opcodes are 32-bit little-endian values at offset 0 of
the payload.

| name               | value | payload layout                                           |
|--------------------|-------|----------------------------------------------------------|
| `DFU_INIT_PACKET`  | `1`   | `u32 op` + init_packet bytes + `u16 0x0000` (padding)    |
| `DFU_START_PACKET` | `3`   | `u32 op` + `u32 mode` + `u32 sd_size` + `u32 bl_size` + `u32 app_size` |
| `DFU_DATA_PACKET`  | `4`   | `u32 op` + firmware chunk, up to 512 bytes                |
| `DFU_STOP_DATA_PACKET` | `5` | `u32 op` (payload is just the opcode, 4 bytes)           |

Update modes used in `DFU_START_PACKET`:

| name                     | value |
|--------------------------|-------|
| `DFU_UPDATE_MODE_NONE`   | `0`   |
| `DFU_UPDATE_MODE_SD`     | `1`   |
| `DFU_UPDATE_MODE_BL`     | `2`   |
| `DFU_UPDATE_MODE_APP`    | `4`   |

For an **application-only** update we always send `mode = 4` with
`sd_size = 0`, `bl_size = 0`, `app_size = len(firmware.bin)`.

## Acknowledgement

After each packet, the PC reads bytes from the port until two `0xC0`
delimiters have been observed. The enclosed bytes are un-escaped and
the first body byte taken:

```
ack = (body[0] >> 3) & 0x07
```

The Adafruit Python reference `send_packet` only checks that it
received an ACK at all — the classical reliable-transport retry logic
in that function is dead code because `last_ack` is `None` before the
break (see `dfu_transport_serial.py:send_packet`). We match that
behaviour: send one packet, read one ACK frame, move on.

## Transmit sequence (APPLICATION update)

```
1.  Connect at 115200 8N1, no flow control.
2.  HciPacket.sequence_number := 0
3.  Send DFU_START_PACKET(mode=4, sd=0, bl=0, app=app_size)
    Sleep erase_wait_time = ((app_size / 4096) + 1) * 89.7 ms
    Read one ACK frame.
4.  Send DFU_INIT_PACKET(init_packet_bytes + [0x00, 0x00])
    Read one ACK frame.
5.  For each 512-byte chunk of firmware.bin:
        Send DFU_DATA_PACKET(chunk)
        Read one ACK frame.
        Every 8th chunk: sleep 0.128 ms (FLASH_PAGE_WRITE_TIME).
6.  Sleep FLASH_PAGE_WRITE_TIME.
7.  Send DFU_STOP_DATA_PACKET()
    Read one ACK frame (may time out if the bootloader activates
    before replying — we treat timeout on STOP as success).
8.  Close the port. The bootloader validates and activates the new
    firmware, then resets into it.
```

## firmware.zip layout

The `firmware.zip` that PlatformIO produces for the Faketec env
(`.pio/build/Faketec/firmware.zip`) contains at least:

- `manifest.json` — metadata (see below)
- `<name>.bin` — raw application image
- `<name>.dat` — init packet bytes for `DFU_INIT_PACKET`

`manifest.json` schema (application update):

```json
{
  "manifest": {
    "application": {
      "bin_file": "firmware.bin",
      "dat_file": "firmware.dat",
      "init_packet_data": {
        "device_type":         <u16>,
        "device_revision":     <u16>,
        "application_version": <u32>,
        "softdevice_req":      [<u16>, ...],
        "firmware_hash":       "<hex sha256 OR crc16>",
        "firmware_length":     <u32>
      }
    }
  }
}
```

The JS client only needs `manifest.application.bin_file` and
`manifest.application.dat_file` to locate the firmware and init
packet bytes inside the zip. The `init_packet_data` block is metadata
for the package generator; the bytes that go on the wire are the raw
contents of the `.dat` file, not a re-encoded copy of this JSON.

## Timing and serial settings

| parameter               | value                                     |
|-------------------------|-------------------------------------------|
| baud rate               | 115200                                    |
| data bits               | 8                                         |
| stop bits               | 1                                         |
| parity                  | none                                      |
| flow control            | none                                      |
| ACK read timeout        | 1.0 s                                     |
| chunk size              | 512 bytes                                 |
| flash page erase time   | 89.7 ms per 4 KB page                     |
| flash page write time   | 0.128 ms (sleep every 8 chunks)           |
| post-data settle        | 0.128 ms before STOP                      |
| post-activate wait      | reboot is automatic, close the port       |

## Entering DFU mode on the Nice!Nano / Faketec

The Adafruit nRF52 bootloader exposes two distinct CDC devices:

- **Application CDC** — the USB serial port the firmware's
  `SerialConsole` speaks on. Normal runtime port.
- **Bootloader CDC** — a different CDC device that appears only when
  the bootloader is running, speaks the HCI DFU protocol above.

To enter bootloader mode, **double-tap the reset button within ~500 ms**.
The onboard LED typically pulses and the USB device re-enumerates with
a different VID/PID pair. The user must then re-select that new port
in the browser's Web Serial port picker before flashing.
