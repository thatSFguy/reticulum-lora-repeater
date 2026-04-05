// =====================================================================
//  reticulum-lora-repeater / src/main.cpp
//  ------------------------------------------------------------------
//  Top-level setup() and loop(). Deliberately tiny — every real
//  subsystem lives in its own translation unit. Phase 1 intentionally
//  boots the board, blinks the LED, and prints a version banner
//  without pulling in Reticulum. Phase 2 wires up the real transport
//  stack. If you find yourself adding logic here that isn't dispatch,
//  it probably belongs in one of the src/<subsystem>.{h,cpp} modules.
// =====================================================================

#include <Arduino.h>
#include "Config.h"
#include "Led.h"
#include "Radio.h"
#include "Transport.h"
#include "Telemetry.h"
#include "LxmfPresence.h"
#include "SerialConsole.h"

// Compile-time version string so the SerialConsole VERSION command has
// something to print and so the boot banner is searchable in logs.
#ifndef RLR_VERSION
  #define RLR_VERSION "0.1.0-dev"
#endif

static rlr::Config g_config{};

// -------------------------------------------------------------------
// setup() — run once, in order:
//   1. Bring the board out of reset (VEXT + LED + Serial)
//   2. Load config from flash (or defaults on first boot)
//   3. Initialise the radio with that config
//   4. Initialise the Reticulum transport stack on top of the radio
//   5. Initialise the serial provisioning console
//   6. Initialise telemetry + LXMF presence subsystems
// -------------------------------------------------------------------
void setup() {
    Serial.begin(115200);
    // Give USB CDC a moment to enumerate before we print the banner.
    uint32_t wait_start = millis();
    while (!Serial && (millis() - wait_start) < 1500) { delay(10); }

    Serial.println();
    Serial.println("=====================================================");
    Serial.print("  reticulum-lora-repeater ");
    Serial.println(RLR_VERSION);
    Serial.print("  Board: ");
    Serial.println(BOARD_NAME);
    Serial.print("  Radio: ");
    Serial.print(RADIO_CHIP);
    Serial.print(" (");
    Serial.print(RADIO_MODULE);
    Serial.println(")");
    Serial.println("=====================================================");

    rlr::led::init();

    rlr::config::load_or_defaults(g_config);
    Serial.print("  display_name: ");
    Serial.println(g_config.display_name);
    Serial.print("  freq: ");
    Serial.print(g_config.freq_hz);
    Serial.print(" Hz  bw: ");
    Serial.print(g_config.bw_hz);
    Serial.print(" Hz  sf: ");
    Serial.print(g_config.sf);
    Serial.print("  cr: ");
    Serial.print(g_config.cr);
    Serial.print("  txp: ");
    Serial.print(g_config.txp_dbm);
    Serial.println(" dBm");

    // --- Phase 2+ wires up real radio + transport here. ---
    // rlr::radio::init_hardware();
    // rlr::radio::begin(g_config);
    // rlr::transport::init(g_config);
    // rlr::telemetry::init(g_config);
    // rlr::lxmf_presence::init(g_config);

    rlr::serial_console::init();

    Serial.println();
    Serial.println("Setup complete. (Phase 1 scaffold — radio + transport not yet wired.)");
    Serial.println("Type HELP over serial for commands.");
}

// -------------------------------------------------------------------
// loop() — cooperative scheduler for all periodic tasks. Every tick()
// must be non-blocking; any subsystem that needs to wait should do
// so via millis()-based state machines, never delay().
// -------------------------------------------------------------------
void loop() {
    // rlr::transport::tick();     // microReticulum scheduler slot (Phase 2)
    // rlr::telemetry::tick(g_config);      // (Phase 5)
    // rlr::lxmf_presence::tick(g_config);  // (Phase 5)
    rlr::led::heartbeat_tick(g_config);
    rlr::serial_console::tick();
}
