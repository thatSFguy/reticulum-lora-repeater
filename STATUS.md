# Status

Quick snapshot for anyone (or future me) picking this up cold. The
plan lives in the sibling project at
`../microReticulum_Faketec_Repeater/reticulum_lora_repeater_PLAN.md`;
this file just tracks "what's actually done right now."

## Current state — end of Phase 2

**Bench-verified working on real hardware** (Faketec, 2026-04-05):

- Firmware boots, prints banner, loads identity + path table from
  internal flash, registers LoRaInterface with Reticulum Transport,
  receives + decodes + rebroadcasts mesh announces
- microStore segment rotation works across firmware upgrades
  (identity persisted from the sibling project's boot carried over
  intact into this repo's first boot)
- 3 path entries preserved from pre-existing `/path_store_0.dat`
- Heap pool: 5% used / 90% free / 0% fragmented, 0 alloc faults
- `[alive]` heartbeat firing every 10 s as expected
- LED heartbeat pulse working

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
