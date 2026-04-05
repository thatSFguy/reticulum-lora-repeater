#pragma once
// src/Telemetry.h — periodic battery / health announces on a custom
// Reticulum aspect (`rlr.telemetry`). Ported from the sibling project's
// announce_telemetry() with Config-driven cadence and calibration.

#include "Config.h"

namespace rlr { namespace telemetry {

// Create the telemetry destination under the Transport identity.
// Must be called AFTER transport::init().
bool init(const Config& cfg);

// Called from loop(); fires the first announce after
// cfg.tele_interval_ms is first reached, then at the configured
// cadence. Honors cfg.flags & CONFIG_FLAG_TELEMETRY.
void tick(const Config& cfg);

// Build and send one telemetry announce immediately. Exposed so the
// serial console can force one for testing without waiting.
void announce_now(const Config& cfg);

// Return the averaged raw ADC reading on PIN_BATTERY without applying
// the cfg.batt_mult scaling. Used by the serial console's CALIBRATE
// BATTERY command to derive a multiplier from a user-supplied
// measured voltage: batt_mult = measured_mv / raw_avg.
// Returns 0 if no battery pin is defined for this board.
uint32_t read_battery_raw_avg();

// Return the averaged raw ADC reading scaled to mV via cfg.batt_mult.
// Equivalent to what announce_now() embeds as "bat=" in the payload.
uint16_t read_battery_mv(const Config& cfg);

}} // namespace rlr::telemetry
