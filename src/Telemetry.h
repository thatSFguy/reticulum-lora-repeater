#pragma once
// src/Telemetry.h — periodic battery / health telemetry as spec-compliant
// LXMF messages. The node packs a Sideband-compatible Telemeter snapshot
// into LXMF FIELD_TELEMETRY (0x02) and sends it to a configured collector
// destination, so it shows up natively in Sideband / MeshChat telemetry
// views (instead of the old custom `rlr.telemetry` announce that
// spec-compliant clients filtered out by name_hash).

#include "Config.h"

namespace rlr { namespace telemetry {

// One-time init. No persistent Reticulum destination is created here any
// more (telemetry is now an LXMF message to a collector, not an announce
// on our own aspect). Kept for symmetry with main::setup() and to log
// readiness. Returns true.
bool init(const Config& cfg);

// Called from loop(); fires the first send shortly after boot, then at
// cfg.tele_interval_ms cadence. No-op unless CONFIG_FLAG_TELEMETRY is
// set, the radio is online, and a collector is configured.
void tick(const Config& cfg);

// Build and send one telemetry message immediately. Exposed so the
// serial console can force one for testing without waiting. Returns true
// if a message was handed to the radio.
bool send_now(const Config& cfg);

// Return the averaged raw ADC reading on PIN_BATTERY without applying
// the cfg.batt_mult scaling. Used by the serial console's CALIBRATE
// BATTERY command to derive a multiplier from a user-supplied measured
// voltage: batt_mult = measured_mv / raw_avg. Returns 0 if no battery
// pin is defined for this board.
uint32_t read_battery_raw_avg();

// Return the averaged raw ADC reading scaled to mV via cfg.batt_mult.
uint16_t read_battery_mv(const Config& cfg);

}} // namespace rlr::telemetry
