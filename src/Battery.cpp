// src/Battery.cpp — averaged ADC read with Config-driven scaling.
// Ported from read_battery_mv() in the sibling project.
#include "Battery.h"
#include <Arduino.h>

namespace rlr { namespace battery {

#ifndef BATTERY_SAMPLES
  #define BATTERY_SAMPLES 16
#endif

uint16_t read_mv(float mult) {
#if defined(PIN_BATTERY) && PIN_BATTERY >= 0
  #if defined(BATTERY_ADC_RESOLUTION)
    analogReadResolution(BATTERY_ADC_RESOLUTION);
  #endif
    uint32_t sum = 0;
    for (int i = 0; i < BATTERY_SAMPLES; i++) {
        sum += analogRead(PIN_BATTERY);
    }
    uint32_t avg = sum / BATTERY_SAMPLES;
    return (uint16_t)((float)avg * mult);
#else
    (void)mult;
    return 0;
#endif
}

}} // namespace rlr::battery
