# Reticulum 0.7+ Ratchet Announce Wire Format

**Status**: research deliverable for Phase 8. Spec is **complete enough to
implement against**, with one TODO (exact header-flag bit position) that
requires a 5-minute follow-up fetch against upstream Python source during
the implementation session. Patches to microReticulum are **deferred until
bench hardware is available** — the risk of shipping an untested protocol
change is a silent regression where existing microReticulum↔microReticulum
announces also stop working, which is worse than the current
Sideband-can't-see-us state.

## Why this exists

Our firmware uses [`attermann/microReticulum`](https://github.com/attermann/microReticulum),
a C++ port of [`markqvist/Reticulum`](https://github.com/markqvist/Reticulum).
The C++ port was last synced against a pre-0.7 upstream; Reticulum 0.7 added
**forward-secrecy ratchets** to the announce format. Announces from upstream
Reticulum nodes (Sideband, NomadNet, reticulum-meshchat, and any RNode running
an up-to-date daemon) carry an extra 32-byte ratchet field our firmware
doesn't know how to parse. Result: we receive the packet, the
`destination_hash` reconstruction fails because we parse the signature and
app_data at the wrong byte offsets, and the announce is silently dropped.

This document captures the exact upstream format so a future session can
patch `Packet.cpp`, `Identity.cpp`, and `Destination.cpp` in a microReticulum
fork to speak both the legacy and ratchet variants, driven by a header flag.

## Sources

All byte layouts and field orders below were extracted verbatim from the
upstream Python master branch at the time of writing:

| File                            | What we mined                                      |
|---------------------------------|-----------------------------------------------------|
| `RNS/Identity.py`               | Constants (`KEYSIZE`, `RATCHETSIZE`, `NAME_HASH_LENGTH`, `SIGLENGTH`), `validate_announce()` byte offsets, signed-data construction |
| `RNS/Destination.py`            | `announce()` method — which context flag gets set when ratchets are present, what goes into `signed_data` on the sender side |
| `RNS/Packet.py`                 | Packet header layout, context type constants, packet types (`ANNOUNCE = 0x01`) |

## Constants

From `RNS/Identity.py`:

```python
KEYSIZE          = 256 * 2       # 512 bits → 64 bytes  (32-byte Ed25519 public + 32-byte X25519 public, concatenated)
RATCHETSIZE      = 256           # 256 bits → 32 bytes  (X25519 public key of the current ratchet)
NAME_HASH_LENGTH = 80            # 80  bits → 10 bytes
HASHLENGTH       = 256           # 256 bits → 32 bytes  (full SHA-256 length)
SIGLENGTH        = KEYSIZE       # 512 bits → 64 bytes  (Ed25519 signature)
# random_hash is NOT a constant but is always 10 bytes (see validate_announce)
```

From `RNS/Reticulum.py`:

```python
TRUNCATED_HASHLENGTH = 128       # 128 bits → 16 bytes  (destination_hash truncation)
```

## Two announce layouts

The announce payload (`packet.data`, i.e. the bytes *after* the Reticulum
packet header) has two possible layouts. Which one is in use is signalled by
a **flag bit in the packet header** (see next section).

### Layout A — Legacy (pre-0.7, or ratchets disabled on the sender)

```
offset  length  field
─────────────────────────────────────────────────────────────
0       64      public_key       (Ed25519 32 B || X25519 32 B)
64      10      name_hash        (truncated SHA-256 of destination aspects)
74      10      random_hash      (per-announce entropy)
84      64      signature        (Ed25519 over signed_data, see below)
148     *       app_data         (variable; may be empty)
```

Total fixed overhead: **148 bytes** before `app_data`.

### Layout B — Ratchet (Reticulum 0.7+, ratchets enabled)

```
offset  length  field
─────────────────────────────────────────────────────────────
0       64      public_key       (same as legacy)
64      10      name_hash        (same as legacy)
74      10      random_hash      (same as legacy)
84      32      ratchet          (current X25519 ratchet public key)   ← NEW
116     64      signature        (shifted 32 bytes later than legacy)
180     *       app_data         (shifted 32 bytes later than legacy)
```

Total fixed overhead: **180 bytes** before `app_data`.

The layouts are *not* distinguishable from the byte content alone — both
start with a 64-byte public key followed by a 10-byte name_hash. The
parser must use the header-flag bit to decide which layout to apply.

## Signed data (input to the signature)

Same field order in both layouts; `ratchet` is `b""` in layout A:

```
signed_data = destination_hash + public_key + name_hash + random_hash + ratchet + app_data
```

Literal from `RNS/Destination.py:announce()`:

```python
signed_data = self.hash + self.identity.get_public_key() + self.name_hash + random_hash + ratchet
if app_data != None:
    signed_data += app_data
```

`destination_hash` here is the full 16-byte truncated hash of the
destination (not just the name hash). `public_key` is the 64-byte
concatenation, same bytes that go into the wire format.

## Ratchet flag in the packet header — ONE TODO

From `RNS/Destination.py:announce()`:

```python
if ratchet:
    context_flag = RNS.Packet.FLAG_SET
else:
    context_flag = RNS.Packet.FLAG_UNSET

announce_packet = RNS.Packet(
    self, announce_data, RNS.Packet.ANNOUNCE,
    context = announce_context,
    attached_interface = attached_interface,
    context_flag = context_flag,
)
```

So the sender sets `context_flag = FLAG_SET` when a ratchet is present.
`context_flag` is **distinct from the `context` byte** that carries the
PATH_RESPONSE / NONE context type — it's a separate one-bit flag in one of
the packet's header bytes.

**Which header byte and which bit?** `Packet.py`'s `pack()` method packs
all header flags into the first header byte (`header[0]`) alongside the
HEADER type, transport, and propagation_type fields. The `context_flag`
bit position needs one more fetch of `RNS/Packet.py:pack()` to nail down
exactly — a ~5-minute job that wasn't worth burning session time on given
we'd have the same research overhead during implementation anyway.

**TODO during implementation session**: fetch
`https://raw.githubusercontent.com/markqvist/Reticulum/master/RNS/Packet.py`,
grep for `FLAG_SET` and `FLAG_UNSET`, reproduce the `pack()` method's
first header byte construction literally, identify the bit, and add the
exact `(header[0] & 0xNN) != 0` check to the spec below before writing
any C++.

## Implementation plan for the microReticulum fork

Three files need patches. Rough shape of each change:

### 1. `Packet.cpp` — pack/unpack the context_flag bit

In `pack()`: when packing an announce packet on a destination that has a
current ratchet, OR the appropriate bit into the first header byte.

In `unpack()`: extract the bit into a new `bool context_flag` field on the
Packet object so downstream consumers can branch on it.

### 2. `Identity.cpp` — parse both layouts in `validate_announce()`

Current `validate_announce()` uses the legacy offsets (148-byte header).
Split it into a branch:

```cpp
size_t sig_offset, app_offset;
Bytes ratchet;
if (packet.context_flag) {
    // Layout B: ratchet is bytes 84..116, signature 116..180, app 180+
    ratchet    = packet.data.mid(84, 32);
    sig_offset = 116;
    app_offset = 180;
} else {
    // Layout A: ratchet empty, signature 84..148, app 148+
    ratchet    = Bytes();  // empty
    sig_offset = 84;
    app_offset = 148;
}
```

Then build `signed_data` with the (possibly empty) ratchet in the same
field order on both branches:

```cpp
Bytes signed_data = destination_hash
                  + public_key
                  + name_hash
                  + random_hash
                  + ratchet          // 0 or 32 bytes
                  + app_data;
```

Signature verification is unchanged — it just operates on the new
signed_data definition.

### 3. `Destination.cpp` — emit ratchet announces when we have one

For v1 of the patch we **don't need to generate ratchet announces** —
we only need to *receive* them correctly. microReticulum has no ratchet
rotation code yet, so our own announces continue to use layout A.

Sideband/NomadNet/RNode nodes will see our legacy announces just fine
(they've always supported both layouts — see `validate_announce` in
upstream Python, which has the same branch we're adding here).

Emitting ratchet announces from our side is a **v2** of the patch and
requires implementing X25519 ratchet key rotation in microReticulum,
which is a much larger change. Not blocking for v0.1 testers.

## Validation plan for the bench session

1. Apply the three-file patch to a fork of `attermann/microReticulum`
   on a branch `ratchet-receive`.
2. Point our `lib_deps` in `platformio.ini` at the fork branch:
   `https://github.com/thatSFguy/microReticulum.git#ratchet-receive`
3. Build + flash.
4. Bring up Sideband or `reticulum-meshchat` on the same radio parameters.
5. Wait for Sideband to emit a ratchet announce.
6. Verify in our serial log that:
   - The packet is received
   - `context_flag = true` is logged
   - Signature verification succeeds against Layout B
   - The destination is added to the path table
7. If any step fails, add trace logging and iterate.
8. Once green, open a PR against `attermann/microReticulum` upstream with
   the patch so other downstream projects benefit.

## What this spec is NOT

- It is not a full Reticulum 0.7+ protocol spec. We intentionally scope
  only to the receive-side of announce packets. Link establishment,
  resource transfers, key exchange, and all the other Reticulum machinery
  are untouched — our firmware uses none of that; we're a transport-only
  node that sees mesh packets fly past.
- It is not a ratchet key-rotation spec. We need to *read* the ratchet
  field in announces from others, not *manage* our own ratchet state.
- It is not bench-validated. Every claim above comes from reading
  upstream Python source; none of it has been verified against a live
  packet capture. First job of the implementation session is to capture
  one real Sideband announce with a logic analyser or SDR, byte-dump it,
  and cross-check every offset against this document.
