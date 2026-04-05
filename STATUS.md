# Status

Quick snapshot for anyone (or future me) picking this up cold. The
plan lives in the sibling project at
`../microReticulum_Faketec_Repeater/reticulum_lora_repeater_PLAN.md`;
this file just tracks "what's actually done right now."

## Current state — end of Phase 2 (RadioLib-based radio, validated RX)

**Bench-verified working on TWO different hardware configurations**
(2026-04-05):

1. **Faketec** — Nice!Nano-style ProMicro clone + Ebyte E22-900M30S
   (SX1262 with external PA, ~30 dBm antenna output)
2. **Test board** — same ProMicro clone carrier + Seeed Wio-SX1262
   (SX1262, ~22 dBm antenna, no external PA)

Both run the **exact same firmware binary** from `[env:Faketec]`
with the same `include/board/Faketec.h`. Per-module differences are
entirely absorbed by RadioLib + the board header macros. This is
the first real test of the one-header-per-board architecture and
it passes.

**Important caveat from the Phase 2 retrospective:** an earlier
"Phase 2 done" claim (commit `f416735`) was wrong. It was based on
the Faketec's microStore path table surviving across firmware
upgrades — the 3 persisted path entries from the old sibling
firmware made it look like live RX was working, but a clean boot
on the Wio-SX1262 (which had no prior state) proved the RX path
was actually broken on both boards. The real fix landed in
commit `084c9c9` (`setRfSwitchPins(RXEN)` for the external LNA
enable on E22/Wio SX1262 modules) and was validated at commit
`084c9c9+` when the Wio board received a path-response packet
27 seconds after reflash (`pin: 0 → pin: 1`). See
docs/TROUBLESHOOTING.md items #12-#15 for the rabbit-hole cascade
that preceded the fix.

## Known limitation — upstream Reticulum ratchet announces

Announces from upstream RNodes running Reticulum ≥ 0.7 are received
and cryptographically verified, but then rejected by microReticulum
with `[DBG] Received invalid announce for <hash>: Destination
mismatch.` This is NOT a bug in this repo.

Root cause: Reticulum 0.7 added forward-secrecy "ratchet" keys to
the announce wire format. Upstream RNodes emit these ratchet-enabled
announces. microReticulum (attermann's C++ port) has partial stub
support — the `RATCHETSIZE` constant is defined but
`Identity::validate_announce()` still parses the announce under the
pre-ratchet byte layout, so its local destination-hash reconstruction
fails the consistency check even though the Ed25519 signature
validates. See the `// CBA RATCHET ... TODO` comment in the libdeps
copy of `microReticulum/src/Packet.cpp` for the explicit author
acknowledgement.

Impact on this firmware:

- ✅ **Peer-to-peer with other microReticulum nodes** (this repo, or
  `microReticulum_Faketec_Repeater`) — announces validate, paths
  build correctly, full bidirectional routing.
- ❌ **Auto-learning paths from upstream Reticulum 0.7+ peers** —
  their announces are heard + verified but rejected at validation,
  so `paths:` does not populate from them.
- ✅ **Forwarding data packets to/from either type of peer** — data
  packet framing hasn't changed in 0.7, so if a path is known (via
  other means or a microReticulum peer), forwarding works fine.

Resolution paths (in order of increasing effort):

1. File an issue at github.com/attermann/microReticulum asking for
   ratchet-aware announce parsing. No work on our side.
2. Wait for an upstream microReticulum release that implements
   ratchet, then bump `lib_deps` in our `platformio.ini`. Zero
   work on our side once the release lands.
3. Patch microReticulum locally to handle the new announce layout.
   Requires reading Python Reticulum's `Identity.py::validate_announce`
   carefully and porting the changes. Estimated 4-8 hours.

For now: **accept and defer**. Phase 2 is complete. Full upstream
interop is gated on an upstream library fix, not a bug in this repo's
code. Revisit after Phase 3-5 land or when microReticulum publishes
a ratchet-capable release.

## Phase 5 subsystems (telemetry, LXMF presence, heartbeat)

All three are **deliberately stubbed out** in the current firmware.
`src/Telemetry.{h,cpp}`, `src/LxmfPresence.{h,cpp}`, and the
telemetry/lxmf ticks in `main.cpp::loop()` are placeholders with
TODO markers pointing at the sibling project's working implementations
to copy. They were scheduled for Phase 5 (after Phase 3-4 provide
the runtime Config and serial console they both depend on).

The sibling project's `RNode_Firmware.ino` has ready-to-port code for:

- `read_battery_mv()` — averaged ADC read on PIN_BATTERY with
  configurable multiplier (Phase 3 field: `Config::batt_mult`)
- `announce_telemetry()` — builds the ASCII `bat=N;up=N;hpf=N;ro=N;
  rssi=N;nf=N` payload and calls `telemetry_destination.announce()`
- `announce_lxmf_presence()` — hand-rolled msgpack encoder for
  `[display_name_bytes, nil]` and the lxmf.delivery announce
- The three periodic tick patterns (telemetry, lxmf presence,
  LED heartbeat)

Can be ported in an afternoon once Phase 3 lands Config persistence.

Bench signals on both boards:

- Firmware boots, prints banner, loads identity + path table from
  internal flash (if present), registers LoRaInterface with
  Reticulum Transport, receives + decodes + rebroadcasts mesh
  announces
- microStore segment rotation works across firmware upgrades
  (identity persisted from the sibling project's boot carried over
  intact into this repo's first boot on the Faketec)
- On a virgin flash (test board), a fresh identity is generated
  and persisted on first boot
- Heap pool: 5% used / 90-91% free / 0% fragmented, 0 alloc faults
- `[alive]` heartbeat firing every 10 s as expected
- LED heartbeat pulse working
- Identical BSS size across both boards (21924 B) — proves shared
  code path, no board-conditional state being compiled in

**Bug discovered and fixed during second-board validation:**
`sx126x::reset()` must be called before `preInit()` probe. The E22
tolerates skipping this step; the Wio does not. See
`docs/TROUBLESHOOTING.md` item #12 and commit `28b2179`.

## Commits

| SHA | Phase | Note |
|---|---|---|
| `73835bd` | 1 | Project scaffold — empty modules, board headers, docs |
| `0b0dfd6` | 2 | Radio + Transport — first working build (silent boot) |
| `a609b55` | 2.1 | Temporary default config override for bench test |
| `f628f17` | 2.2 | Enable RNS_USE_FS + register_filesystem (fixed one silent-boot cause) |
| `8519c89` | 2.3 | **filesystem.init() call** — fixed the actual silent-boot bug |

Bench-validated at `8519c89`. The boot sequence before it was broken
for a non-obvious reason: `register_filesystem()` alone is not enough
— the `microStore::FileSystem` impl must have `init()` called on it
explicitly to mount the littlefs partition. Every microReticulum
example does this two-step init + register, but the sibling project
had the `filesystem.init()` call ~30 lines earlier in its setup()
than the register call so it was easy to miss on a port that looked
only at the register site. See commit message for details.

## Known simplifications in Phase 2

These are intentional and will be addressed in later phases:

- **No packet fragmentation.** Payloads > 255 bytes are dropped.
  Reticulum defaults fit under this at SF10/250 kHz so it's fine
  for v0.1. Fragmentation would be its own subsystem.
- **No CSMA / DCD / airtime tracking.** TX is synchronous
  `beginPacket → write → endPacket` through the driver, no channel
  utilization awareness.
- **Single-packet RX latch.** ISR stages one packet; `tick()` drains
  and feeds it to the interface. Back-to-back RX might drop the
  second packet.
- **Hardcoded defaults.** `DEFAULT_CONFIG_*` in `Faketec.h` currently
  matches the production mesh (904.375 MHz / 250 kHz / SF10 / CR5 /
  22 dBm) as a bench-test override. Revert to US ISM 915 MHz defaults
  after Phase 3-4 lands runtime provisioning.
- **Stubs for Telemetry, LxmfPresence, SerialConsole.** Phase 4-5.

## Phase 3-7 roadmap

From the plan doc:

- **Phase 3** — Config load/save to microStore, replaces hardcoded
  defaults with a real persisted runtime config
- **Phase 4** — Serial provisioning console
  (`CONFIG GET/SET/COMMIT`, `STATUS`, `VERSION`, etc.)
- **Phase 5** — Telemetry + LXMF presence + heartbeat ported from
  the sibling project and parameterized by Config
- **Phase 6** — Webflasher (fresh build, JS-native DFU, config form,
  battery calibration helper)
- **Phase 7** — Second board port (RAK4631 or XIAO) to validate the
  one-header-per-board claim

## Not yet committed to the remote

This repo is local-only at `C:\Users\rob\PlatformIO\reticulum-lora-repeater\`.
Push to `github.com/thatSFguy/reticulum-lora-repeater` once Phase 3
is working at minimum. Pushing earlier risks "the public HEAD was
never actually functional for anyone who cloned it" scenarios.
