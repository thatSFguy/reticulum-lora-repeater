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
#include "Storage.h"
#include "Led.h"
#include "Radio.h"
#include "Transport.h"
#include "Telemetry.h"
#include "LxmfPresence.h"
#include "SerialConsole.h"
#include "Ble.h"

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
    // Give USB CDC more time to enumerate. nRF52 TinyUSB doesn't replay
    // buffered bytes to a late-attach monitor, so if the user starts
    // `pio device monitor` after the wait elapses they miss the banner.
    // Extending to 8 s gives plenty of headroom for a human to attach.
    // We also emit a dot every 500 ms so anyone already attached sees
    // continuous proof that setup() is running.
    uint32_t wait_start = millis();
    uint32_t last_dot = 0;
    while (!Serial && (millis() - wait_start) < 8000) {
        if (millis() - last_dot >= 500) {
            last_dot = millis();
            Serial.print('.');
            Serial.flush();
        }
        delay(10);
    }
    Serial.println();

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

    // Mount the internal flash filesystem before anything that might
    // touch persistent storage — specifically Config::load_or_defaults
    // below needs it so /config.bin can be read or created. Transport
    // also depends on it later, but storage::init() only needs to run
    // once and it's idempotent from main's perspective.
    rlr::storage::init();

    rlr::config::load_or_defaults(g_config);
    Serial.println("---- Active config ----");
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

    // --- BLE (before radio — available even if radio fails) ---
    rlr::ble::init(g_config);

    // --- Radio + Reticulum transport ---
    // Strict order:
    //   1. init_hardware()  — VEXT + SPI pins + chip reset + sync-word probe
    //   2. begin(cfg)       — configure chip (freq/BW/SF/CR/TXP/CRC), leave in STANDBY
    //   3. transport::init()— filesystem, register RX callback (wires chip IRQ
    //                          mask routing and host DIO1 interrupt BEFORE the
    //                          chip ever enters RX mode), start Reticulum
    //   4. radio::start_rx()— NOW enter continuous RX. Must come after step 3.
    //
    // If any step fails we still enter loop() so the serial console is
    // available for diagnosis.
    if (rlr::radio::init_hardware()) {
        if (rlr::radio::begin(g_config)) {
            if (rlr::transport::init(g_config)) {
                rlr::radio::start_rx();
                // Phase 5: telemetry + LXMF presence destinations are
                // stamped against the Transport identity, so they
                // MUST be created after transport::init() has called
                // reticulum.start() and the identity is loaded.
                rlr::telemetry::init(g_config);
                rlr::lxmf_presence::init(g_config);
            } else {
                Serial.println("Setup: transport::init() failed — radio staying in standby");
            }
        } else {
            Serial.println("Setup: radio::begin() failed — transport not started");
        }
    } else {
        Serial.println("Setup: radio::init_hardware() failed — transport not started");
    }

    rlr::serial_console::init(g_config);

    Serial.println();
    Serial.println("Setup complete.");
    Serial.println("Type HELP over serial for commands.");
}

// -------------------------------------------------------------------
// loop() — cooperative scheduler for all periodic tasks. Every tick()
// must be non-blocking; any subsystem that needs to wait should do
// so via millis()-based state machines, never delay().
// -------------------------------------------------------------------
void loop() {
    // Pause LoRa radio activity while a BLE device is connected.
    // SX1262 SPI transactions block the MCU and starve the SoftDevice,
    // causing BLE supervision timeout disconnects. The user is
    // configuring during BLE — LoRa can wait.
    if (!rlr::ble::connected()) {
        rlr::transport::tick();
        rlr::telemetry::tick(g_config);
        rlr::lxmf_presence::tick(g_config);
    }
    rlr::led::heartbeat_tick(g_config);
    rlr::serial_console::tick();
    rlr::ble::tick();

    // Periodic alive marker — provides at-a-glance proof the firmware
    // is running and basic stats for a late-attach serial monitor.
    // Suppressed at log_level 0 (quiet).
    static uint32_t last_alive_ms = 0;
    uint32_t now = millis();
    if (g_config.log_level >= 1 && (int32_t)(now - last_alive_ms) >= 10000) {
        last_alive_ms = now;
        Serial.print("[alive] uptime=");
        Serial.print(now / 1000);
        Serial.print("s radio=");
        Serial.print(rlr::radio::online() ? "up" : "down");
        Serial.print(" pin=");
        Serial.print(rlr::transport::packets_in());
        Serial.print(" pout=");
        Serial.print(rlr::transport::packets_out());
        Serial.println();
    }
}
