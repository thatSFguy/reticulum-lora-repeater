#pragma once
// src/Battery.h — single-shot averaged ADC read on PIN_BATTERY,
// scaled by Config.batt_mult to return millivolts. Used by Telemetry.

#include <stdint.h>

namespace rlr { namespace battery {

// Read N samples and return the averaged voltage in millivolts.
// `mult` is typically Config.batt_mult (ADC-raw → mV scaling factor
// calibrated against a multimeter).
uint16_t read_mv(float mult);

}} // namespace rlr::battery
