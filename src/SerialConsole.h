#pragma once
// src/SerialConsole.h — line-oriented provisioning protocol over USB
// CDC. Commands:
//
//   PING | VERSION | STATUS | HELP | REBOOT
//   CONFIG GET | CONFIG SET <key> <value> | CONFIG RESET | CONFIG COMMIT
//
// Every response ends with a line "OK" or "ERR: <reason>". The
// webflasher (and any terminal) parses until it sees one of these.
// CONFIG SET is staged in RAM; CONFIG COMMIT persists and reboots.

#include "Config.h"

namespace rlr { namespace serial_console {

// Initialise (no-op for now, reserved for Phase 4).
void init();

// Called every loop tick. Reads available serial bytes, splits on
// newline, dispatches commands. Non-blocking.
void tick();

// Direct access to the staging Config the console edits. main.cpp
// uses this on first boot to seed the stage from disk before
// opening the console for reads/writes.
Config& staging();

}} // namespace rlr::serial_console
