# Receiving RLR Repeater Telemetry — Integration Guide

This document describes, in implementation-agnostic terms, how the RLR LoRa
repeater firmware emits telemetry and exactly what a receiving application must
do to recognize, decrypt, attribute, and decode it. Hand this to the agent
implementing telemetry support in the receiving app.

It assumes the receiving app already has a working Reticulum/LXMF stack
(identity, announces, opportunistic LXMF delivery). Everything here is built on
standard Reticulum + LXMF + the Sideband "Telemeter" field convention — there is
**no custom packet type and no custom announce aspect** to special-case.

---

## 1. The one-sentence summary

The repeater sends its telemetry as a **standard LXMF message** addressed to a
single configured collector, carrying a **Sideband Telemeter snapshot** in LXMF
**field key `2` (`FIELD_TELEMETRY`)**. To consume it, your app must be the
addressed recipient, then look for field `2` in delivered LXMF messages.

---

## 2. What you CANNOT do (common wrong assumptions)

- **You cannot find it in announces / a node list.** Earlier firmware put
  telemetry in announce `app_data` as a `key=value;` text beacon. That was
  removed: per Reticulum SPEC §4.4 a non-LXMF announce aspect is a "custom
  beacon" that spec-compliant clients drop from their UI. The repeater now
  announces only its normal `lxmf.delivery` destination. **No telemetry travels
  in announces.** Any "telemetry on nodes" view that parses announce `app_data`
  will never see this data.

- **You cannot passively sniff and filter it.** The message body is
  Token-encrypted to the collector's identity. Only the addressed recipient can
  decrypt it. "Filtering" is therefore something you do *after* delivery to your
  own destination — not a promiscuous capture filter.

**Consequence:** the first requirement is that the repeater's `collector` config
is set to **your app's `lxmf.delivery` destination hash**, and your app has
announced so the repeater could recall its identity and encrypt to it. Without
that, nothing arrives.

---

## 3. Recognizing a telemetry message

After your LXMF stack delivers and decrypts a message, decode the LXMF payload.
An LXMF message payload is msgpack:

```
[ timestamp(float64), title(bin), content(bin), fields(map) , stamp? ]
```

A message is a telemetry message **iff its `fields` map contains the integer key
`2`** (`0x02`, `FIELD_TELEMETRY`, defined in LXMF SPEC §5.9.1).

```
isTelemetry(message) := message.fields.containsKey(2)
```

Notes:
- The key is an integer `2`. Depending on your msgpack decoder it may arrive as
  any integer type — compare numerically (`key.toInt() == 2`), not by object
  identity or string.
- `content` and `title` are typically empty on these messages; do not rely on
  them. All telemetry lives in `fields[2]`.

The value of `fields[2]` is a msgpack **binary blob** (a nested, independently
msgpack-encoded map — the Telemeter snapshot). You must run your msgpack decoder
a **second time** on those bytes.

---

## 4. Attributing telemetry to a specific repeater

Every LXMF message carries a 16-byte **`source_hash`** (the sender's
`lxmf.delivery` destination hash; in the opportunistic wire form it is the first
16 bytes of the message body, before the 64-byte signature). This is the stable
per-node identifier.

```
filter: message.source_hash == <repeaters lxmf.delivery hash>  AND  2 in message.fields
```

Each repeater's `lxmf.delivery` hash is derived from its identity as
`Destination.hash(identity, "lxmf", "delivery")` and is visible in the
repeater's announce. Treat it as the device's unique ID; the human-readable name
(e.g. `Rptr-DC6E07432975`) comes from the repeater's `lxmf.delivery` announce
`app_data` display name, not from the telemetry.

---

## 5. Decoding the Telemeter snapshot (`fields[2]`)

`fields[2]` decodes to a msgpack **map keyed by integer Sensor IDs (SIDs)**. This
is the Sideband "Telemeter" format (`sbapp/sideband/sense.py`). The repeater
emits a subset; ignore unknown SIDs gracefully.

| SID  | Name          | msgpack value shape                                             |
|------|---------------|----------------------------------------------------------------|
| 0x01 | TIME          | `uint` — seconds                                               |
| 0x02 | LOCATION      | `array[7]` (see below) — present only if a location is configured |
| 0x04 | BATTERY       | `array[3]` (see below) — present only if a battery is read     |
| 0x0F | INFORMATION   | `str` — free-form human-readable stats                         |

TIME and INFORMATION are always present. LOCATION and BATTERY are conditional.

### 5.1 SID 0x01 — TIME
- A single unsigned integer of **seconds**.
- ⚠️ This hardware has **no RTC/GPS wall clock**. The value is **monotonic
  uptime seconds**, not Unix epoch time. Do not display it as a date. Render
  telemetry against your own receive timestamp (this matches how Sideband
  behaves). Use it only for relative ordering / "device uptime."

### 5.2 SID 0x02 — LOCATION
A 7-element array. Each numeric element is a big-endian fixed-point integer that
sense.py wraps as a msgpack **`bin`** (raw bytes), NOT as a msgpack integer. You
must read the bytes and reassemble them yourself:

| idx | field       | encoding                                  | to real value          |
|-----|-------------|-------------------------------------------|------------------------|
| 0   | latitude    | 4-byte big-endian **signed** int (`bin`)  | `value / 1e6` degrees  |
| 1   | longitude   | 4-byte big-endian **signed** int (`bin`)  | `value / 1e6` degrees  |
| 2   | altitude    | 4-byte big-endian **signed** int (`bin`)  | `value / 100` metres   |
| 3   | speed       | 4-byte big-endian **signed** int (`bin`)  | `value / 100` (0 here) |
| 4   | bearing     | 4-byte big-endian **signed** int (`bin`)  | `value / 100` (0 here) |
| 5   | accuracy    | 2-byte big-endian **unsigned** int (`bin`)| `value / 100` (0 here) |
| 6   | last_update | msgpack `uint`                            | seconds (uptime, as TIME) |

The repeater fills speed/bearing/accuracy with 0 (it is stationary and has no
GNSS). Only latitude, longitude, and altitude are meaningful, and only when the
operator configured a fixed position; otherwise the LOCATION SID is omitted
entirely.

Decoding a `bin` field, e.g. latitude:
```
bytes = fields[2][2][0]            // 4 raw bytes, big-endian
raw   = int32_be(bytes)            // signed 32-bit, MSB first
lat   = raw / 1_000_000.0          // degrees
```

### 5.3 SID 0x04 — BATTERY
A 3-element array:

| idx | field          | msgpack type | meaning                                  |
|-----|----------------|--------------|------------------------------------------|
| 0   | charge_percent | `float64`    | 0.0–100.0, 0.1% resolution               |
| 1   | charging       | `bool`       | always `false` (not detectable on HW)    |
| 2   | temperature    | `nil`        | not available                            |

`charge_percent` is a **coarse linear estimate** from terminal voltage
(3.30 V → 0 %, 4.20 V → 100 %), suitable as an indicator, not a fuel gauge.
Present only when a battery voltage is readable.

### 5.4 SID 0x0F — INFORMATION
A single UTF-8 string of free-form repeater stats that have no dedicated SID, of
the form:

```
up=<uptime_s>s heap=<bytes> pin=<packets_in> pout=<packets_out> bat=<mV>mV radio=<up|down>
```

Display it verbatim, or parse the `key=value` tokens if you want structured
fields. Treat the format as informational and subject to change.

---

## 6. Decoded example (structural)

A delivered telemetry message decodes to roughly this (shown as JSON for
clarity; the wire is msgpack, and `fields[2]` is itself a nested msgpack blob):

```jsonc
// LXMF payload
[
  1234.5,                 // timestamp (uptime seconds, NOT epoch)
  "",                     // title (empty)
  "",                     // content (empty)
  {                       // fields
    2: <bin>              // FIELD_TELEMETRY → nested msgpack below
  }
]

// fields[2] decoded (the Telemeter snapshot)
{
  1: 1234,                          // TIME: uptime seconds
  2: [                              // LOCATION (only if configured)
       <bin 4B>,  // lat  raw/1e6
       <bin 4B>,  // lon  raw/1e6
       <bin 4B>,  // alt  raw/100
       <bin 4B>,  // speed   (0)
       <bin 4B>,  // bearing (0)
       <bin 2B>,  // accuracy(0)
       1234       // last_update (uptime seconds)
     ],
  4: [ 87.5, false, null ],         // BATTERY: percent, charging, temp
  15: "up=1234s heap=48000 pin=28 pout=12 bat=3900mV radio=up"  // INFORMATION
}
```

---

## 7. Implementation checklist

1. **Be the collector.** Ensure your app has a stable `lxmf.delivery`
   destination and announces it. Tell the operator to set the repeater's
   `collector` config to your app's `lxmf.delivery` hash. (You cannot receive
   telemetry otherwise — it is encrypted to that destination.)
2. **Deliver + verify** the incoming LXMF message through your normal
   opportunistic-delivery path (decrypt, then verify the Ed25519 signature per
   LXMF §5.5 — `source_hash + dest_hash + payload + SHA256(...)`). Reject
   messages that fail signature verification.
3. **Detect telemetry:** `2 in message.fields`.
4. **Attribute:** key the telemetry by `message.source_hash` (the repeater's
   `lxmf.delivery` hash).
5. **Decode `fields[2]`** with a second msgpack pass → SID-keyed map.
6. **Extract SIDs** you care about (§5). Skip unknown SIDs without erroring.
7. **Render against your own receive clock** — the device TIME is uptime, not
   wall-clock. Battery/charging/temperature have the documented limitations.
8. **Store/update** a per-`source_hash` "latest telemetry" record; optionally
   keep history. Map coordinates come from the LOCATION SID when present.

---

## 8. Reference

- Repeater sender: `src/Telemetry.cpp` (Telemeter build), `src/Lxmf.cpp`
  (opportunistic LXMF framing + signing).
- LXMF message format & signing: Reticulum LXMF SPEC §5.1, §5.3, §5.5, §5.7.1.
- `FIELD_TELEMETRY` (key `0x02`): LXMF SPEC §5.9.1.
- Telemeter SID definitions & value encodings: Sideband
  `sbapp/sideband/sense.py` (upstream source of truth).
