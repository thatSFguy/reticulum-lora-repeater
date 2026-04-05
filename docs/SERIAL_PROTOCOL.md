# Serial provisioning protocol

A deliberately trivial line-oriented text protocol over USB CDC at
115200 8N1. Both the webflasher (via Web Serial API) and a human in
a terminal program can talk to it. **No KISS, no binary framing, no
escape sequences.**

Every command ends with `\n` or `\r\n`. Every response ends with a
line consisting of exactly `OK` or `ERR: <reason>`. The client reads
until it sees either terminator.

## Commands

### `PING`
→ `PONG`
→ `OK`

Liveness check.

### `VERSION`
→ `reticulum-lora-repeater <semver> <board>`
→ `OK`

Example: `reticulum-lora-repeater 0.1.0 Faketec`

### `STATUS`
→ `uptime=<s> radio=<0|1> paths=<n> dsts=<n> sram_free=<bytes> batt_mv=<mV>`
→ `OK`

One-shot live status for the webflasher post-provision verification.

### `CONFIG GET`
→ one `key=value` line per config field
→ `OK`

Lists every field in the live runtime config (not the staged one
unless no staging has occurred yet). Keys are stable identifiers the
webflasher form uses.

### `CONFIG SET <key> <value>`
→ `OK` or `ERR: <reason>`

Writes to the **staging** config in RAM. Nothing hits flash until
`CONFIG COMMIT`. Multiple SETs in sequence are atomic from the
commit's perspective: either they all persist or none do.

Quoted values are accepted for strings with spaces:

```
CONFIG SET display_name "Solar Site A"
```

Validation happens on each SET; out-of-range values return
`ERR: <reason>` and leave the staging copy unchanged.

Valid keys:

| Key | Type | Range |
|---|---|---|
| `freq_hz` | uint32 | 100,000,000 – 1,100,000,000 |
| `bw_hz` | uint32 | 7800 – 500000 |
| `sf` | uint8 | 7 – 12 |
| `cr` | uint8 | 5 – 8 (denominator of 4/5..4/8) |
| `txp_dbm` | int8 | -9 – +22 |
| `flags` | uint8 | bitmask; see Config.h |
| `batt_mult` | float | 0.1 – 10.0 |
| `tele_interval_ms` | uint32 | 60000 – 86400000 |
| `lxmf_interval_ms` | uint32 | 60000 – 7200000 |
| `display_name` | string | up to 31 bytes UTF-8 |

### `CONFIG RESET`
→ `OK`

Restores the staging copy to the board's hardcoded defaults. Does
**not** touch flash. Follow with `CONFIG COMMIT` to persist.

### `CONFIG COMMIT`
→ `COMMITTING`
→ `OK`
→ (node reboots ~1 s later)

Validates the staging copy, computes its CRC, writes to microStore,
and issues a soft reboot so the new config takes effect via the
normal boot path. **The webflasher should wait ~3 s and then
reconnect to USB CDC** to verify via `STATUS`.

### `REBOOT`
→ `OK`
→ (node reboots)

Soft reboot without touching config.

### `HELP`
→ (prints the command list and key list)
→ `OK`

## Webflasher example session

```
→ PING
← PONG
← OK
→ VERSION
← reticulum-lora-repeater 0.1.0 Faketec
← OK
→ CONFIG RESET
← OK
→ CONFIG SET freq_hz 904375000
← OK
→ CONFIG SET bw_hz 250000
← OK
→ CONFIG SET sf 10
← OK
→ CONFIG SET cr 5
← OK
→ CONFIG SET txp_dbm 22
← OK
→ CONFIG SET display_name "Roof Site North"
← OK
→ CONFIG GET
← version=1
← freq_hz=904375000
← bw_hz=250000
← sf=10
← cr=5
← txp_dbm=22
← flags=0x07
← batt_mult=1.284
← tele_interval_ms=10800000
← lxmf_interval_ms=1800000
← display_name=Roof Site North
← OK
→ CONFIG COMMIT
← COMMITTING
← OK
   (wait 3 s for reboot + reconnect)
→ STATUS
← uptime=4 radio=1 paths=0 dsts=3 sram_free=72100 batt_mv=5012
← OK
```

## Design notes

- Lines starting with `#` are comments and are ignored. Lets a
  provisioning file have inline annotations.
- Unknown commands return `ERR: unknown command <name>`. No silent
  success; forces webflasher to keep its command list in sync.
- All numeric values are decimal; hex is accepted with `0x` prefix
  for bitmask fields like `flags`.
- Strings with spaces must be quoted with `"..."`. Internal escapes
  are not supported in v0.1 — keep display names ASCII-adjacent.
