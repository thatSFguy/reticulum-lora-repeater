# Reticulum 0.7+ Ratchet Announce Wire Format

**Status**: implemented and building green on both Faketec and
RAK4631. Applied as a pre-build string-replacement patch in
`scripts/pre_build.py` (see `patch_microreticulum()`), targeting
`.pio/libdeps/<env>/microReticulum/src/Identity.cpp`. **Bench
validation against a live Sideband instance is still pending** —
this spec and patch ship from code review against the upstream
Python source and microReticulum C++ port, not from a captured
real-world announce. First user to run this against a Sideband mesh
should confirm announces are received, signature check succeeds,
and the destination hash is added to the path table.

**Risk profile**: defensive-only. The legacy code path (`context_flag
= 0`, which is every pre-ratchet peer including our own outgoing
announces) is **bit-identical** to the upstream microReticulum
implementation — same byte offsets, same `signed_data` construction
(operator<< with an empty `Bytes` is a no-op). Worst case if the
ratchet path has a bug: Sideband announces are still rejected, same
as before the patch. It cannot regress existing
microReticulum↔microReticulum mesh traffic.

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

## Ratchet flag in the packet header — resolved

From upstream `RNS/Packet.py` (literal):

```python
FLAG_SET       = 0x01
FLAG_UNSET     = 0x00

def get_packed_flags(self):
    ...
    packed_flags = (self.header_type << 6) | (self.context_flag << 5) | ...

# unpack:
self.context_flag = (self.flags & 0b00100000) >> 5
```

`context_flag` is **bit 5** of the first header byte. Upstream
microReticulum (`attermann/microReticulum`) **already handles this
correctly** in `Packet.cpp:get_packed_flags()` and `unpack_flags()`
— the same `(_context_flag << 5)` pack and `(flags & 0b00100000) >> 5`
unpack as upstream Python. The `Packet` class even exposes a
`context_flag()` accessor. The plumbing is all there; the original
author just never wired it through to `Identity::validate_announce`,
leaving a `// CBA RATCHET` TODO comment in `Packet.cpp:353` that
marks where the rest of the ratchet work was intended.

Our pre-build patch (`scripts/pre_build.py:patch_microreticulum()`)
teaches `validate_announce` to read `packet.context_flag()` and
branch on it. No header files or type definitions need to change —
the `RATCHETSIZE = 256` constant was already reserved in
`Type.h:172` when the port author anticipated this work.

## Implementation — shipped

Scope turned out narrower than initially scoped: only **one file, one
function** needed patching. Header/unpack plumbing was already in
place in upstream microReticulum.

### The actual change

`Identity::validate_announce` in
`.pio/libdeps/<env>/microReticulum/src/Identity.cpp`, lines
420-465ish. The upstream function uses legacy byte offsets
unconditionally. Our patch introduces a `has_ratchet` branch driven
by `packet.context_flag()`:

```cpp
const bool has_ratchet = (packet.context_flag() == Type::Packet::FLAG_SET);
const size_t RATCHET_BYTES = has_ratchet ? (RATCHETSIZE/8) : 0;

Bytes public_key  = packet.data().left(KEYSIZE/8);
Bytes name_hash   = packet.data().mid(KEYSIZE/8, NAME_HASH_LENGTH/8);
Bytes random_hash = packet.data().mid(KEYSIZE/8 + NAME_HASH_LENGTH/8, RANDOM_HASH_LENGTH/8);

Bytes ratchet;
if (has_ratchet) {
    ratchet = packet.data().mid(
        KEYSIZE/8 + NAME_HASH_LENGTH/8 + RANDOM_HASH_LENGTH/8,
        RATCHETSIZE/8);
}

Bytes signature = packet.data().mid(
    KEYSIZE/8 + NAME_HASH_LENGTH/8 + RANDOM_HASH_LENGTH/8 + RATCHET_BYTES,
    SIGLENGTH/8);

// app_data slicing is also offset-shifted by RATCHET_BYTES
// ...

Bytes signed_data;
signed_data << packet.destination_hash() << public_key << name_hash
            << random_hash << ratchet << app_data;
```

When `has_ratchet = false`: `RATCHET_BYTES` is 0, `ratchet` is an
empty `Bytes`, every slice offset collapses to the upstream legacy
math, and `signed_data << empty` is a no-op (verified by reading
`Bytes::operator<<` → `append(bytes)` in `Bytes.h`). **Bit-identical
to the upstream code path** for every announce our firmware and
existing microReticulum peers emit today.

When `has_ratchet = true`: slices 32 extra bytes between random_hash
and signature, and splices the ratchet into `signed_data` in the
field position upstream Python uses in `validate_announce`.

### Why no sender-side changes (yet)

microReticulum has no ratchet key rotation machinery, so our own
announces still go out in layout A. That's fine for v1 because
Sideband / NomadNet / upstream Python Reticulum all support
receiving BOTH layouts — their own `validate_announce` has the
same `if has_ratchet` branch we just added.

Emitting ratchet announces from our side is a **future project**
requiring X25519 ratchet key generation, rotation timers, and
persistence. Not needed for this phase's goal of "nodes visible in
Sideband".

### Why a pre-build patch instead of a fork

Zero external dependencies. No fork to maintain, no `lib_deps` pin
to update, no user action required to create a fork. The patch
lives in `scripts/pre_build.py` as a plain string replacement
against the fetched lib source, with:

  * An idempotency marker (`// RLR_RATCHET_PATCH`) so repeated
    builds are no-ops
  * A hard failure if the upstream source ever drifts from what
    we targeted (so we catch silent patch breakage at build time
    rather than shipping an unpatched firmware)

When it's time to upstream this, the replacement block text IS the
PR against `attermann/microReticulum`.

## Validation plan for the bench session

1. Flash the current firmware (v0.1.1+ includes the ratchet patch).
2. Open the serial console and verify boot shows no new errors.
3. Bring up Sideband or `reticulum-meshchat` on the same radio parameters
   (freq, BW, SF, CR).
4. Wait for Sideband to emit an announce — typically within a minute of
   app start, or force one from the Sideband UI.
5. Check the path table growth in our STATUS output:
   - Before patch: `paths=0` indefinitely for Sideband peers
   - After patch: `paths` counter should tick up when a Sideband announce
     is processed
6. If paths don't populate but the radio is hearing packets (`pin` is
   ticking), un-comment the `TRACEF` lines in the patched `validate_announce`
   to see exactly where the validation is failing, and compare the
   `signed_data` hex against what an upstream Python reference dump
   would produce.
7. Once confirmed working, open a PR against `attermann/microReticulum`
   upstream with the patch so other downstream projects benefit.
   The PR is literally the replacement block text from
   `scripts/pre_build.py:PATCHED_BLOCK` pasted into the upstream file.

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
