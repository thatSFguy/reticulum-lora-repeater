#pragma once
// src/SerialConsole.h — line-oriented provisioning protocol over USB
// CDC (and BLE UART). Commands:
//
//   PING | VERSION | STATUS | HELP | REBOOT | DFU
//   CONFIG GET | CONFIG SET <key> <value>
//   CONFIG RESET | CONFIG REVERT | CONFIG COMMIT
//
// Every response ends with a line "OK" or "ERR: <reason>". The
// webflasher (and any terminal) parses until it sees one of these.
// CONFIG SET is staged in RAM; CONFIG COMMIT persists and reboots.

#include "Config.h"
#include <Print.h>

namespace rlr { namespace serial_console {

// Wire the console up to the live Config that main.cpp owns. The
// staging copy the user edits is seeded from this reference; on
// COMMIT we validate, persist via config::save(), and NVIC reset so
// the new settings take effect. Must be called after
// config::load_or_defaults() has populated `live`.
void init(Config& live);

// Called every loop tick. Reads available serial bytes, splits on
// newline, dispatches commands. Non-blocking.
void tick();

// Dispatch a complete command line to the given output stream. This
// is the entry point for BLE UART — the BLE module accumulates a
// line, then calls this with its BlePrint adapter.
void dispatch_line(const char* line, Print& out);

// Direct access to the staging Config the console edits. Exposed
// mainly for tests and for STATUS to introspect uncommitted edits.
Config& staging();

}} // namespace rlr::serial_console
