// src/Radio.cpp — SX1262 radio lifecycle. Wraps the ported sx126x
// driver from src/drivers/ and drives it from Config + board-header
// macros. Replaces the monolithic startRadio() / setTXPower() /
// setBandwidth() / setSpreadingFactor() / setCodingRate() fanout
// from the sibling project's RNode_Firmware.ino + Utilities.h.

#include "Radio.h"
#include "drivers/sx126x.h"
#include <Arduino.h>
#include <SPI.h>

namespace rlr { namespace radio {

// File-scope driver instance — the sx126x class owns the SPI + GPIO
// state for the radio chip. There is exactly one LoRa radio per
// repeater so a single instance is fine.
static sx126x s_lora;
static bool   s_online = false;

bool init_hardware() {
    // 1. Gate the radio 3V3 rail on boards that have a VEXT_EN pin.
    //    Without this on Faketec the SX1262 is physically unpowered
    //    when we try to talk to it over SPI. Match MeshCore's 10 ms
    //    settle delay so the TCXO has time to start oscillating.
    #if HAS_VEXT_RAIL && defined(PIN_VEXT_EN) && PIN_VEXT_EN >= 0
        pinMode(PIN_VEXT_EN, OUTPUT);
        digitalWrite(PIN_VEXT_EN, HIGH);
        delay(VEXT_SETTLE_MS);
    #endif

    // 2. Wire the driver to its pins (CS, RESET, DIO1, BUSY, RXEN).
    //    SPI pin setup (MISO/SCK/MOSI) happens inside the driver's
    //    preInit() — it reads RADIO_SPI_OVERRIDE_PINS and calls
    //    SPI.setPins() before SPI.begin() when the flag is set.
    s_lora.setPins(PIN_LORA_NSS, PIN_LORA_RESET, PIN_LORA_DIO1,
                   PIN_LORA_BUSY, PIN_LORA_RXEN);

    // 3. Probe the chip. preInit() resets the radio, begins SPI,
    //    and reads the sync-word register in a 2-second retry loop
    //    to confirm the chip is responding. Returns false if the
    //    chip never shows up — usually a wiring, power, or TCXO
    //    voltage problem (see docs/TROUBLESHOOTING.md).
    if (!s_lora.preInit()) {
        Serial.println("Radio: preInit() failed — SX1262 did not respond");
        return false;
    }

    Serial.println("Radio: hardware init OK");
    return true;
}

bool begin(const Config& cfg) {
    // Full chip init — TCXO (voltage chosen via RADIO_TCXO_VOLTAGE_MV
    // in the board header), sync word, LoRa mode, standby, PA config.
    if (s_lora.begin((long)cfg.freq_hz) != 1) {
        Serial.println("Radio: begin() failed");
        s_online = false;
        return false;
    }

    // Apply Config radio parameters.
    s_lora.setSignalBandwidth((long)cfg.bw_hz);
    s_lora.setSpreadingFactor((int)cfg.sf);
    s_lora.setCodingRate4((int)cfg.cr);
    s_lora.setTxPower((int)cfg.txp_dbm);      // SX1262: outputPin arg ignored
    s_lora.enableCrc();

    // Enter continuous RX mode. The driver also drives RXEN high
    // before entering RX if pin_rxen is configured.
    s_lora.receive();

    s_online = true;
    Serial.print("Radio: online @ ");
    Serial.print(cfg.freq_hz);
    Serial.print(" Hz, BW=");
    Serial.print(cfg.bw_hz);
    Serial.print(" Hz, SF=");
    Serial.print(cfg.sf);
    Serial.print(", CR=4/");
    Serial.print(cfg.cr);
    Serial.print(", TXP=");
    Serial.print(cfg.txp_dbm);
    Serial.println(" dBm");
    return true;
}

bool online() { return s_online; }

void stop() {
    s_lora.sleep();
    s_online = false;
}

// Allow other subsystems (e.g. Transport's LoRaInterface) to access
// the driver instance directly. Declared in Radio.h.
sx126x& driver() { return s_lora; }

}} // namespace rlr::radio
