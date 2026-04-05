// src/Radio.cpp — SX1262 radio wrapping RadioLib's SX1262 + Module.
//
// Everything in this file is board-agnostic. Pin numbers, TCXO
// voltage, and RF-switch wiring come from the pre-included board
// header macros (PIN_LORA_*, RADIO_TCXO_VOLTAGE_MV, RADIO_DIO2_AS_RF_SWITCH).
// Adding a new SX1262-based board is purely a new
// include/board/<name>.h file — nothing in this driver changes.

#include "Radio.h"
#include <Arduino.h>
#include <SPI.h>
#include <RadioLib.h>

namespace rlr { namespace radio {

// ---- File-scope state ---------------------------------------------

// The RadioLib Module object owns the CS/reset/IRQ/busy pin config
// and the SPI handle for one physical radio chip. SX1262 is a thin
// class that takes a Module* and provides the high-level API. Both
// live at file scope so their constructors run during static init
// (after Arduino core is up but before setup()).
static Module s_module(PIN_LORA_NSS,   // CS / NSS
                       PIN_LORA_DIO1,  // IRQ (DIO1 on SX1262)
                       PIN_LORA_RESET, // RESET
                       PIN_LORA_BUSY); // BUSY
static SX1262 s_radio(&s_module);

static bool s_online = false;

// ISR flag set by RadioLib's packet-received callback. Volatile
// because it's written from interrupt context and read from loop().
// RadioLib's setPacketReceivedAction wires the SX1262's RX_DONE
// chip-internal IRQ to DIO1 AND attaches this handler via
// attachInterrupt() under the hood — both steps happen atomically
// so there's no ordering hazard like the old driver had.
static volatile bool s_rx_flag = false;
static void isr_packet_received() {
    s_rx_flag = true;
}

// ---- API ----------------------------------------------------------

bool init_hardware() {
    // 1. Gate the external 3V3 rail on boards that require it.
    //    Without this on Faketec (VEXT_EN = P0.13) the SX1262 is
    //    physically unpowered when we try to talk to it.
    #if HAS_VEXT_RAIL && defined(PIN_VEXT_EN) && PIN_VEXT_EN >= 0
        pinMode(PIN_VEXT_EN, OUTPUT);
        digitalWrite(PIN_VEXT_EN, HIGH);
        delay(VEXT_SETTLE_MS);
    #endif

    // 2. Override the Arduino SPI default pins if the board's
    //    PlatformIO variant doesn't match our wiring (e.g. Faketec
    //    piggy-backs on pca10056 whose default MOSI=45 collides
    //    with our CS). Must happen BEFORE SPI.begin().
    #if defined(RADIO_SPI_OVERRIDE_PINS) && RADIO_SPI_OVERRIDE_PINS
        SPI.setPins(PIN_LORA_MISO, PIN_LORA_SCK, PIN_LORA_MOSI);
    #endif
    SPI.begin();

    Serial.println("Radio: init_hardware OK (VEXT + SPI pins ready)");
    return true;
}

bool begin(const Config& cfg) {
    // RadioLib takes frequency in MHz (float), bandwidth in kHz
    // (float), SF, CR denominator, sync word, power dBm, preamble
    // length, TCXO voltage in volts, and a boolean for regulator LDO
    // vs DC-DC selection. Translate our Config struct into that
    // vocabulary.
    float freq_mhz = (float)cfg.freq_hz / 1000000.0f;
    float bw_khz   = (float)cfg.bw_hz   / 1000.0f;

    // Sync word: SX1262 expects a single byte that RadioLib expands
    // internally to the 16-bit register using Semtech AN1200.48's
    // mapping. 0x12 is the RNode / private-LoRa sync word (maps to
    // 0x1424 at the register level) and is what the sibling project
    // and the rest of our Reticulum mesh already use.
    const uint8_t sync_word      = 0x12;
    const uint16_t preamble_len  = 18;    // matches upstream RNode + microReticulum defaults
    const bool use_regulator_ldo = false; // DC-DC regulator (default for most modules)

    // TCXO voltage from the board header. RADIO_TCXO_VOLTAGE_MV is
    // 1800 for Ebyte E22 / Seeed Wio modules, 3300 for RAK4631.
    // Pass 0 to disable TCXO usage if the board has no TCXO.
    float tcxo_v = 0.0f;
    #if HAS_TCXO && defined(RADIO_TCXO_VOLTAGE_MV)
        tcxo_v = (float)RADIO_TCXO_VOLTAGE_MV / 1000.0f;
    #endif

    int state = s_radio.begin(freq_mhz, bw_khz, (uint8_t)cfg.sf,
                              (uint8_t)cfg.cr, sync_word,
                              (int8_t)cfg.txp_dbm, preamble_len,
                              tcxo_v, use_regulator_ldo);
    if (state != RADIOLIB_ERR_NONE) {
        Serial.print("Radio: SX1262 begin() failed, RadioLib code ");
        Serial.println(state);
        return false;
    }

    // DIO2 drives the external RF switch on Ebyte E22 and Wio-SX1262
    // modules — RadioLib handles the TX/RX antenna path automatically
    // once this is set.
    #if RADIO_DIO2_AS_RF_SWITCH
        state = s_radio.setDio2AsRfSwitch(true);
        if (state != RADIOLIB_ERR_NONE) {
            Serial.print("Radio: setDio2AsRfSwitch failed, RadioLib code ");
            Serial.println(state);
        }
    #endif

    // Enable the LNA's boosted gain mode. Costs ~2 mA idle but adds
    // ~3 dB of sensitivity in RX — worthwhile for a repeater.
    s_radio.setRxBoostedGainMode(true);

    s_online = true;
    Serial.print("Radio: configured @ ");
    Serial.print(cfg.freq_hz);
    Serial.print(" Hz, BW=");
    Serial.print(cfg.bw_hz);
    Serial.print(" Hz, SF=");
    Serial.print(cfg.sf);
    Serial.print(", CR=4/");
    Serial.print(cfg.cr);
    Serial.print(", TXP=");
    Serial.print(cfg.txp_dbm);
    Serial.print(" dBm, TCXO=");
    Serial.print(tcxo_v, 2);
    Serial.println(" V");
    return true;
}

bool start_rx() {
    if (!s_online) {
        Serial.println("Radio: start_rx() called before begin()");
        return false;
    }
    // Register our ISR flag-setter and enter continuous RX. RadioLib
    // configures the SX1262's IRQ mask routing (RX_DONE → DIO1) AND
    // attaches the host-side interrupt handler, atomically. No
    // ordering hazard between these two steps.
    s_radio.setPacketReceivedAction(isr_packet_received);
    int state = s_radio.startReceive();
    if (state != RADIOLIB_ERR_NONE) {
        Serial.print("Radio: startReceive() failed, RadioLib code ");
        Serial.println(state);
        return false;
    }
    Serial.println("Radio: entering continuous RX");
    return true;
}

bool online()     { return s_online; }
bool rx_pending() { return s_rx_flag; }

void stop() {
    s_radio.standby();
    s_online = false;
}

int read_pending(uint8_t* buf, size_t bufsize) {
    if (!s_rx_flag) return 0;
    s_rx_flag = false;

    size_t len = s_radio.getPacketLength();
    if (len == 0 || len > bufsize) {
        // Bad length — re-enter RX anyway so we don't get stuck.
        s_radio.startReceive();
        return -1;
    }
    int state = s_radio.readData(buf, len);

    // Re-enter continuous RX for the next packet before we return,
    // so there's never a window where the chip is idle waiting for
    // the application to tell it what to do next.
    s_radio.startReceive();

    return (state == RADIOLIB_ERR_NONE) ? (int)len : -1;
}

int transmit(const uint8_t* buf, size_t len) {
    if (!s_online) return -1;
    // RadioLib's transmit() is blocking — it puts the chip in TX
    // mode, waits for TX_DONE, and then returns. After TX we must
    // explicitly re-enter continuous RX (transmit() doesn't do that
    // for us).
    int state = s_radio.transmit(const_cast<uint8_t*>(buf), len);
    s_radio.startReceive();
    return (state == RADIOLIB_ERR_NONE) ? (int)len : -1;
}

}} // namespace rlr::radio
