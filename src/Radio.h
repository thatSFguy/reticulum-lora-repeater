#pragma once
// src/Radio.h — SX1262 radio lifecycle and parameter application.
// Wraps src/drivers/sx126x.{h,cpp} and consumes board constants from
// the pre-included board header.

#include "Config.h"

namespace rlr { namespace radio {

// Assert PIN_VEXT_EN (if the board has it), initialise SPI with the
// board's pin map, reset + preInit the SX1262, verify the chip
// responds to a sync-word read. Returns true if the radio is online
// and ready for begin().
bool init_hardware();

// Apply the runtime Config's frequency/BW/SF/CR/TXP to the radio and
// enter continuous RX mode. Call after init_hardware() and after any
// config change. Returns true on success.
bool begin(const Config& cfg);

// Query whether the radio is currently online and receiving.
bool online();

// Force the radio into standby (used by shutdown paths).
void stop();

}} // namespace rlr::radio
