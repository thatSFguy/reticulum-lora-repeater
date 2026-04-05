#pragma once
// src/Led.h — single generic LED driver parameterized by the
// board header's PIN_LED and LED_ACTIVE_HIGH macros. Also provides
// the non-blocking heartbeat tick used for "this node is alive"
// visual feedback on deployed nodes.

#include "Config.h"

namespace rlr { namespace led {

void init();

// Activity indication (called from radio RX/TX callbacks).
void on();
void off();

// Non-blocking heartbeat pulse. Call from loop(); honors
// cfg.flags & CONFIG_FLAG_HEARTBEAT.
void heartbeat_tick(const Config& cfg);

}} // namespace rlr::led
