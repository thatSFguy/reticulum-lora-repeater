// src/Led.cpp — minimal polarity-aware LED driver. Replaces the
// per-board led_rx_on/off/led_tx_on/off copy-paste pattern from the
// sibling project.
#include "Led.h"
#include <Arduino.h>

namespace rlr { namespace led {

#if defined(PIN_LED) && PIN_LED >= 0
  #if defined(LED_ACTIVE_HIGH) && LED_ACTIVE_HIGH
    #define LED_LVL_ON  HIGH
    #define LED_LVL_OFF LOW
  #else
    #define LED_LVL_ON  LOW
    #define LED_LVL_OFF HIGH
  #endif
#endif

void init() {
#if defined(PIN_LED) && PIN_LED >= 0
    pinMode(PIN_LED, OUTPUT);
    digitalWrite(PIN_LED, LED_LVL_OFF);
#endif
}

void on() {
#if defined(PIN_LED) && PIN_LED >= 0
    digitalWrite(PIN_LED, LED_LVL_ON);
#endif
}

void off() {
#if defined(PIN_LED) && PIN_LED >= 0
    digitalWrite(PIN_LED, LED_LVL_OFF);
#endif
}

void heartbeat_tick(const Config& cfg) {
#if defined(PIN_LED) && PIN_LED >= 0
    if ((cfg.flags & CONFIG_FLAG_HEARTBEAT) == 0) return;
    // TODO Phase 5: port the non-blocking 15s/8ms pulse from the
    // sibling project's loop() directly, using cfg-configurable
    // interval + duration fields (future schema addition).
    static uint32_t next_on  = 15000;
    static uint32_t off_at   = 0;
    const uint32_t interval_ms = 15000;
    const uint32_t duration_ms = 8;
    uint32_t now = millis();
    if (off_at != 0 && (int32_t)(now - off_at) >= 0) {
        digitalWrite(PIN_LED, LED_LVL_OFF);
        off_at = 0;
    }
    if ((int32_t)(now - next_on) >= 0) {
        digitalWrite(PIN_LED, LED_LVL_ON);
        off_at  = now + duration_ms;
        next_on = now + interval_ms;
    }
#else
    (void)cfg;
#endif
}

}} // namespace rlr::led
