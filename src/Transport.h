#pragma once
// src/Transport.h — Reticulum transport stack lifecycle.
// Wraps microReticulum's RNS::Transport / RNS::Reticulum /
// RNS::Identity plumbing and exposes a narrow interface to main.cpp.

#include "Config.h"

namespace rlr { namespace transport {

// Initialize the microReticulum stack: filesystem, RNG, Identity,
// Transport, Reticulum instance, LoRaInterface registration,
// persistent destinations for local operations. Must be called
// AFTER radio::begin() so the LoRaInterface has a working radio.
// Returns true on success.
bool init(const Config& cfg);

// Called every loop tick to give microReticulum its cooperative
// scheduler slot. Safe to call at high frequency.
void tick();

// Getters for the StatusLine on the serial console.
uint32_t path_count();
uint32_t destination_count();
uint32_t packets_in();
uint32_t packets_out();

}} // namespace rlr::transport
