// src/Telemetry.cpp — periodic telemetry as spec-compliant LXMF messages.
//
// Previously this emitted an ASCII "bat=...;up=..." payload as an announce
// on a custom `rlr.telemetry` aspect. Per reticulum-specifications
// SPEC.md §4.4, any non-LXMF name_hash is a "custom beacon" that
// spec-compliant clients (Sideband, MeshChat, Columba) drop from their
// UI — which is exactly why issue #1's user saw only lxmf.delivery and no
// telemetry.
//
// Telemetry in the Reticulum ecosystem is an LXMF message field, not an
// announce: FIELD_TELEMETRY (0x02) carrying a Sideband "Telemeter"
// snapshot (SPEC §5.9.1; the inner Telemeter format comes from upstream
// Sideband `sbapp/sideband/sense.py`). We build that snapshot and push it
// to a configured collector via opportunistic LXMF delivery (Lxmf.cpp).
//
// Telemeter snapshot = msgpack map { sensor_SID: packed_value, ... }:
//   SID_TIME        0x01  int   (unix seconds; uptime here — no RTC)
//   SID_LOCATION    0x02  [lat,lon,alt,speed,bearing,accuracy,last_update]
//                         where lat/lon/alt/... are big-endian struct
//                         ints wrapped as msgpack bin (per sense.py)
//   SID_BATTERY     0x04  [charge_percent_f64, charging_bool, temperature]
//   SID_INFORMATION 0x0F  str   (free-form repeater stats with no SID)
// The whole map is msgpack-packed and embedded as the FIELD_TELEMETRY
// value (a nested msgpack `bin`), matching Sideband's Telemeter.packed().

#include "Telemetry.h"
#include "Lxmf.h"
#include "Msgpack.h"
#include "Transport.h"
#include "Radio.h"

#include <Arduino.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <microReticulum/Utilities/Memory.h>

namespace rlr { namespace telemetry {

// Sideband sensor IDs (sbapp/sideband/sense.py).
static constexpr uint8_t SID_TIME        = 0x01;
static constexpr uint8_t SID_LOCATION    = 0x02;
static constexpr uint8_t SID_BATTERY     = 0x04;
static constexpr uint8_t SID_INFORMATION = 0x0F;
// LXMF field key (SPEC §5.9.1).
static constexpr uint8_t FIELD_TELEMETRY = 0x02;

static bool             s_ready    = false;
static uint32_t         s_last_ms  = 0;
static constexpr uint32_t FIRST_MS = 30000UL;   // first send 30 s after init
static constexpr int    BATT_SAMPLES = 16;

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

uint16_t read_battery_mv(const Config& cfg) {
    uint32_t raw = read_battery_raw_avg();
    if (raw == 0) return 0;
    return (uint16_t)((float)raw * cfg.batt_mult);
}

bool init(const Config& cfg) {
    (void)cfg;
    s_ready = true;
    Serial.println("Telemetry: LXMF FIELD_TELEMETRY mode (push to collector)");
    return true;
}

// True if no collector has been configured (all-zero hash).
static bool collector_unset(const Config& cfg) {
    for (size_t i = 0; i < sizeof(cfg.collector_hash); i++) {
        if (cfg.collector_hash[i] != 0) return false;
    }
    return true;
}

// Approximate single-cell LiPo charge percentage from terminal voltage.
// A linear 3.30 V (0%) .. 4.20 V (100%) map — coarse but adequate for a
// telemetry indicator; documented as an estimate. Boards with different
// chemistry/cell counts can be refined later.
static float battery_percent(uint16_t mv) {
    if (mv == 0) return 0.0f;
    float pct = (float)(mv - 3300) / (4200.0f - 3300.0f) * 100.0f;
    if (pct < 0.0f)   pct = 0.0f;
    if (pct > 100.0f) pct = 100.0f;
    return roundf(pct * 10.0f) / 10.0f;   // 0.1% resolution, like Sideband
}

// Emit a big-endian struct int as a msgpack bin (sense.py wraps each
// struct.pack("!i"/"!I"/"!H", ...) result as a Python bytes → msgpack bin).
static void bin_be32(msgpack::Writer& w, uint32_t v) {
    uint8_t b[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16),
                     (uint8_t)(v >> 8),  (uint8_t)v };
    w.bin(b, 4);
}
static void bin_be16(msgpack::Writer& w, uint16_t v) {
    uint8_t b[2] = { (uint8_t)(v >> 8), (uint8_t)v };
    w.bin(b, 2);
}

// Build the Sideband Telemeter snapshot (nested msgpack map).
static void build_telemeter(const Config& cfg, msgpack::Writer& tele) {
    uint32_t now_s   = millis() / 1000UL;
    uint16_t batt_mv = read_battery_mv(cfg);

    bool have_loc = (cfg.latitude_udeg != 0 || cfg.longitude_udeg != 0);
    bool have_bat = (batt_mv > 0);

    size_t nsensors = 2;                     // TIME + INFORMATION always
    if (have_loc) nsensors++;
    if (have_bat) nsensors++;
    tele.map_header(nsensors);

    // SID_TIME → unix seconds. No RTC/GPS clock on this hardware, so this
    // is monotonic uptime; Sideband displays against its own receive time.
    tele.uint(SID_TIME);
    tele.uint(now_s);

    // SID_LOCATION → [lat, lon, alt, speed, bearing, accuracy, last_update]
    if (have_loc) {
        tele.uint(SID_LOCATION);
        tele.array_header(7);
        bin_be32(tele, (uint32_t)cfg.latitude_udeg);          // lat * 1e6 (== udeg)
        bin_be32(tele, (uint32_t)cfg.longitude_udeg);         // lon * 1e6
        bin_be32(tele, (uint32_t)(int32_t)(cfg.altitude_m * 100)); // alt * 1e2
        bin_be32(tele, 0);                                    // speed * 1e2
        bin_be32(tele, 0);                                    // bearing * 1e2
        bin_be16(tele, 0);                                    // accuracy * 1e2
        tele.uint(now_s);                                     // last_update
    }

    // SID_BATTERY → [charge_percent, charging, temperature]
    if (have_bat) {
        tele.uint(SID_BATTERY);
        tele.array_header(3);
        tele.float64(battery_percent(batt_mv));
        tele.boolean(false);                                  // charging unknown
        tele.nil();                                           // temperature unknown
    }

    // SID_INFORMATION → free-form text carrying the repeater stats that
    // have no dedicated Sideband sensor.
    char info[96];
    snprintf(info, sizeof(info),
        "up=%lus heap=%u pin=%lu pout=%lu bat=%umV radio=%s",
        (unsigned long)now_s,
        (unsigned)RNS::Utilities::Memory::heap_available(),
        (unsigned long)rlr::transport::packets_in(),
        (unsigned long)rlr::transport::packets_out(),
        (unsigned)batt_mv,
        rlr::radio::online() ? "up" : "down");
    tele.uint(SID_INFORMATION);
    tele.str(info);
}

bool send_now(const Config& cfg) {
    if (collector_unset(cfg)) {
        Serial.println("Telemetry: no collector configured — set 'collector' to a 32-hex destination hash");
        return false;
    }

    // Build the Telemeter snapshot, then wrap it as the FIELD_TELEMETRY
    // value inside a one-entry LXMF fields map.
    msgpack::Writer tele;
    build_telemeter(cfg, tele);

    msgpack::Writer fields;
    fields.map_header(1);
    fields.uint(FIELD_TELEMETRY);
    fields.bin(tele.data(), tele.size());     // nested Telemeter msgpack

    Serial.print("Telemetry: telemeter=");
    Serial.print((unsigned)tele.size());
    Serial.print("B fields=");
    Serial.print((unsigned)fields.size());
    Serial.println("B");

    return rlr::lxmf::send_opportunistic(
        cfg.collector_hash, /*content=*/"", fields.data(), fields.size());
}

void tick(const Config& cfg) {
    if (!s_ready) return;
    if ((cfg.flags & CONFIG_FLAG_TELEMETRY) == 0) return;
    if (!rlr::radio::online()) return;
    if (collector_unset(cfg)) return;

    uint32_t now = millis();
    bool due;
    if (s_last_ms == 0) {
        due = (now >= FIRST_MS);
    } else {
        due = ((now - s_last_ms) >= cfg.tele_interval_ms);
    }
    if (!due) return;

    s_last_ms = now;
    send_now(cfg);
}

}} // namespace rlr::telemetry
