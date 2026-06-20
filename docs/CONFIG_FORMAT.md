# Config format

## On-disk layout

The runtime config is a single packed C struct persisted via
[microStore](https://github.com/attermann/microStore) under the key
`"config"` in the internal littlefs partition. The file lives
alongside the Reticulum path table at `/config_store_0.dat` (and
rotates via the segment ring like any other microStore data).

Single struct, single record, atomic write. 96 bytes on disk at the
current schema (v3).

```cpp
#pragma pack(push, 1)
struct Config {
    uint16_t version;               // 1 = original, 2 = +BT/location, 3 = +collector
    uint8_t  log_level;             // 0=quiet, 1=normal, 2=verbose
    uint8_t  _reserved;
    uint32_t freq_hz;
    uint32_t bw_hz;
    uint8_t  sf;
    uint8_t  cr;
    int8_t   txp_dbm;
    uint8_t  flags;                 // bit0 telemetry, bit1 lxmf, bit2 heartbeat, bit3 BT
    float    batt_mult;
    uint32_t tele_interval_ms;
    uint32_t lxmf_interval_ms;
    uint32_t bt_pin;                // BLE pairing PIN, 0 = none
    int32_t  latitude_udeg;         // microdegrees (deg * 1e6), 0 = unset
    int32_t  longitude_udeg;        // microdegrees (deg * 1e6), 0 = unset
    int32_t  altitude_m;            // meters MSL, 0 = unset
    char     display_name[32];
    uint8_t  collector_hash[16];    // telemetry collector lxmf.delivery hash (v3)
    uint32_t crc32;
};
#pragma pack(pop)
```

See `src/Config.h` for the canonical definition.

## Field descriptions

| Field | Unit | Notes |
|---|---|---|
| `version` | schema ver | Bumped on incompatible struct changes. Loaders accept older versions and zero-fill missing tail fields. |
| `freq_hz` | Hz | LoRa carrier center frequency. |
| `bw_hz` | Hz | LoRa channel bandwidth. Must match between all peers. |
| `sf` | — | Spreading factor, 7–12. |
| `cr` | — | Coding rate denominator (5..8 → 4/5..4/8). |
| `txp_dbm` | dBm | Target output power at the SX1262 core pin (not the antenna — external PAs add their own gain). Range -9..+22 on SX1262. |
| `flags` | bitmask | Bit 0 = telemetry, 1 = LXMF presence, 2 = heartbeat, 3 = BT enabled. |
| `batt_mult` | — | Raw-ADC → millivolts scaling factor. **Must be calibrated per-board with a multimeter.** |
| `tele_interval_ms` | ms | Between LXMF telemetry pushes to the collector. Default 3 h. |
| `lxmf_interval_ms` | ms | Between LXMF presence announces. Default 30 min. |
| `display_name` | UTF-8 | NUL-terminated, max 31 bytes of content. What MeshChat / Sideband show. |
| `collector_hash` | 16 bytes | Recipient `lxmf.delivery` destination hash for telemetry pushes (`FIELD_TELEMETRY`). All-zero = telemetry disabled. Set via the `collector` key (32 hex chars, or `none` to clear). Added in schema **v3**. |
| `crc32` | — | CRC-32 of all preceding bytes. Corrupt records are rejected and the loader falls back to defaults. |

### Telemetry is LXMF `FIELD_TELEMETRY`, not a custom announce

Telemetry is sent as a spec-compliant LXMF message (opportunistic
delivery, reticulum-specifications SPEC.md §5.1) carrying a Sideband
`Telemeter` snapshot in `FIELD_TELEMETRY` (`0x02`), addressed to the
configured `collector` — so it appears in Sideband / MeshChat's native
telemetry view. Earlier firmware emitted an ASCII beacon on a custom
`rlr.telemetry` aspect, which spec-compliant clients filter out by
`name_hash` (SPEC.md §4.4); that mode has been removed.

The collector's public identity must already be known to the node (it
has heard the collector's announce) before the body can be encrypted to
it; until then telemetry pushes are skipped and logged. The node has no
real-time clock, so the LXMF/telemetry timestamp is monotonic uptime —
receivers display messages against their own receive clock.

## Schema versioning

When a new field is added:

1. Append the new field **before `crc32`**.
2. Bump `version` by 1.
3. Loader code in `src/Config.cpp` must handle the old version: read
   the N bytes for the old struct, zero-fill the new field(s), set
   `version = new`, recompute CRC.
4. Never remove or reorder fields. Deprecate with comments instead.

This keeps forward and backward compatibility trivial: a v2 firmware
reading a v1 config zero-fills the new fields; a v1 firmware reading
a v2 config rejects the CRC and falls back to defaults (cautious but
safe).

## Defaults

Per-board defaults are defined in the pre-included board header via
the `DEFAULT_CONFIG_*` macros. See `include/board/Faketec.h` for the
reference set. These are what a freshly-flashed unit boots with if
no persisted config exists yet.

Safe-by-default philosophy:

- `freq_hz = 915000000` — legal in US ISM, but **may be illegal in
  your region**. The webflasher forces region selection before
  allowing the first commit to catch this.
- `txp_dbm = 14` — conservative; user raises it after confirming
  regional legality.
- `display_name = "Unconfigured Repeater"` — the node literally
  announces that it's unconfigured so nobody ships one to production
  by accident.
