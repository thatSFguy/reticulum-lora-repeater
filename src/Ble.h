#pragma once
// src/Ble.h — BLE UART (Nordic UART Service) for wireless serial
// console access. Exposes the same CONFIG GET/SET/COMMIT protocol
// over Bluetooth that USB CDC provides over the wire.
//
// Guarded by HAS_BLE — when 0 all functions are no-ops and no
// Bluefruit headers are pulled in, keeping the build clean for
// boards that don't want BLE.

#include "Config.h"

namespace rlr { namespace ble {

// Initialize BLE stack and start advertising if bt_enabled flag is
// set in config. Returns true if BLE is active, false if disabled
// or HAS_BLE == 0.
bool init(const Config& cfg);

// Poll BLE UART for incoming bytes, accumulate into a line buffer,
// and dispatch complete lines through the serial console command
// parser. Non-blocking.
void tick();

// Returns true if BLE was initialized and is advertising.
bool active();

}} // namespace rlr::ble
