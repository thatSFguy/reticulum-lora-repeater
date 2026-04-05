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

}} // namespace rlr::telemetry
