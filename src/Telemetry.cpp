// src/Telemetry.cpp — periodic battery/health announces on aspect
// "rlr/telemetry". A thin port of the sibling project's
// announce_telemetry(), with cadence and calibration driven by the
// runtime Config (no more BAKED_* build flags).
//
// Wire format is deliberately trivial: ASCII "key=value;..." stuffed
// into the announce's app_data. Any Reticulum node on the mesh can
// register an announce handler for the aspect "rlr.telemetry" and
// log the payload with no per-node pairing, no key management, and
// no custom framing. See scripts/telemetry_receiver.py (Phase 6
// companion) for the reference decoder.

#include "Telemetry.h"
#include "Transport.h"
#include "Radio.h"

#include <Arduino.h>
#include <stdio.h>

#include <Reticulum.h>
#include <Transport.h>
#include <Destination.h>
#include <Bytes.h>
#include <Utilities/OS.h>
#include <Utilities/Memory.h>

namespace rlr { namespace telemetry {

// File-scope destination. Lives for the life of the program so its
// internal shared_ptr keeps a stable hash, which makes the node
// trivially identifiable from the mesh side across reboots (the
// Transport identity is persisted by microReticulum already).
static RNS::Destination s_dest(RNS::Type::NONE);
static bool             s_ready     = false;
static uint32_t         s_last_ms   = 0;
static constexpr uint32_t FIRST_MS  = 30000UL;   // first announce 30 s after init
static constexpr int     BATT_SAMPLES = 16;

// Average a small burst of ADC samples on PIN_BATTERY. Returned as a
// 32-bit raw value (pre-multiplier) so the serial console's
// CALIBRATE BATTERY command can compute a fresh multiplier from a
// user-supplied measured voltage without a second code path.
uint32_t read_battery_raw_avg() {
#if defined(PIN_BATTERY) && PIN_BATTERY >= 0
  #if defined(BATTERY_ADC_RESOLUTION)
    analogReadResolution(BATTERY_ADC_RESOLUTION);
  #else
    analogReadResolution(12);
  #endif
    uint32_t sum = 0;
    for (int i = 0; i < BATT_SAMPLES; i++) {
        sum += analogRead(PIN_BATTERY);
    }
    return sum / BATT_SAMPLES;
#else
    return 0;
#endif
}

// Scaled reading in mV, what announce_now() embeds as "bat=" in the
// telemetry payload. Per-device calibration against a multimeter is
// expected; the CALIBRATE BATTERY serial command automates it.
uint16_t read_battery_mv(const Config& cfg) {
    uint32_t raw = read_battery_raw_avg();
    if (raw == 0) return 0;
    return (uint16_t)((float)raw * cfg.batt_mult);
}

bool init(const Config& cfg) {
    (void)cfg;
    try {
        // Construct the persistent destination under the Transport
        // identity so its hash is stable across reboots. Must run
        // after transport::init() has called reticulum.start() and
        // the Transport identity has been loaded from flash.
        s_dest = RNS::Destination(
            RNS::Transport::identity(),
            RNS::Type::Destination::IN,
            RNS::Type::Destination::SINGLE,
            "rlr", "telemetry");
        s_ready = true;
        Serial.print("Telemetry: destination hash ");
        Serial.println(s_dest.hash().toHex().c_str());
        return true;
    }
    catch (const std::exception& e) {
        Serial.print("Telemetry::init exception: ");
        Serial.println(e.what());
        s_ready = false;
        return false;
    }
}

void announce_now(const Config& cfg) {
    if (!s_ready) return;
    char buf[80];
    int n = snprintf(buf, sizeof(buf),
        "bat=%u;up=%lu;hpf=%u;ro=%u;pin=%lu;pout=%lu",
        (unsigned)rlr::telemetry::read_battery_mv(cfg),
        (unsigned long)(millis() / 1000UL),
        (unsigned)RNS::Utilities::Memory::heap_available(),
        (unsigned)(rlr::radio::online() ? 1 : 0),
        (unsigned long)rlr::transport::packets_in(),
        (unsigned long)rlr::transport::packets_out());
    if (n <= 0) return;
    Serial.print("Telemetry: ");
    Serial.println(buf);
    try {
        s_dest.announce(RNS::bytesFromString(buf));
    }
    catch (const std::exception& e) {
        Serial.print("Telemetry::announce exception: ");
        Serial.println(e.what());
    }
}

void tick(const Config& cfg) {
    if (!s_ready) return;
    if ((cfg.flags & CONFIG_FLAG_TELEMETRY) == 0) return;
    if (!rlr::radio::online()) return;

    uint32_t now = millis();
    bool due;
    if (s_last_ms == 0) {
        // First-fire window: short (FIRST_MS) so bench-testing
        // doesn't wait out the full cfg.tele_interval_ms (hours).
        due = (now >= FIRST_MS);
    } else {
        due = ((now - s_last_ms) >= cfg.tele_interval_ms);
    }
    if (!due) return;

    s_last_ms = now;
    announce_now(cfg);
}

}} // namespace rlr::telemetry
