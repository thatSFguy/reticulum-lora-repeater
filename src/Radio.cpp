// src/Radio.cpp — SX1262 radio wrapping RadioLib's SX1262 + Module.
//
// Everything in this file is board-agnostic. Pin numbers, TCXO
// voltage, and RF-switch wiring come from the pre-included board
// header macros (PIN_LORA_*, RADIO_TCXO_VOLTAGE_MV, RADIO_DIO2_AS_RF_SWITCH).
// Adding a new SX1262-based board is purely a new
// include/board/<name>.h file — nothing in this driver changes.

#include "Radio.h"
#include "Config.h"
#include <Arduino.h>
#include <SPI.h>
#include <RadioLib.h>

namespace rlr { namespace radio {

// Stored config pointer for radio recovery after SPI failures.
static const Config* s_cfg_ptr = nullptr;

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

// ---- RNode split-packet reassembly state --------------------------
// RNode splits packets > 254 bytes into two LoRa frames with the
// same header byte (upper nibble = sequence, lower bit 0 = FLAG_SPLIT).
// We buffer the first half and join when the second half arrives.
static constexpr uint8_t FLAG_SPLIT = 0x01;
static constexpr size_t  SINGLE_MTU = 255;   // max LoRa frame (header + 254 payload)
static constexpr size_t  MAX_PAYLOAD = 508;   // max reassembled Reticulum payload

static uint8_t  s_split_buf[512];    // buffered first-half payload (no header)
static size_t   s_split_len = 0;     // bytes in split_buf
static uint8_t  s_split_seq = 0xFF;  // sequence nibble of first half (0xFF = none)
static uint32_t s_split_ms  = 0;     // millis() when first half arrived

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
    s_cfg_ptr = &cfg;  // store for radio recovery
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
    const uint16_t preamble_len  = 16;    // MeshCore uses 16; testing interop with SX1276
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

    s_radio.setCRC(1);

    #if RADIO_DIO2_AS_RF_SWITCH
        state = s_radio.setDio2AsRfSwitch(true);
        if (state != RADIOLIB_ERR_NONE) {
            Serial.print("Radio: setDio2AsRfSwitch failed, RadioLib code ");
            Serial.println(state);
        }
    #endif

    #if defined(PIN_LORA_RXEN) && PIN_LORA_RXEN >= 0
        {
            uint32_t tx_pin = RADIOLIB_NC;
            #if defined(PIN_LORA_TXEN) && PIN_LORA_TXEN >= 0
                tx_pin = (uint32_t)PIN_LORA_TXEN;
            #endif
            s_radio.setRfSwitchPins((uint32_t)PIN_LORA_RXEN, tx_pin);
        }
    #endif

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
    // Expire stale split-packet first half (500ms timeout)
    if (s_split_seq != 0xFF && (millis() - s_split_ms) > 500) {
        Serial.println("Radio: split timeout, discarding first half");
        s_split_seq = 0xFF;
        s_split_len = 0;
    }

    if (!s_rx_flag) return 0;
    s_rx_flag = false;

    size_t len = s_radio.getPacketLength();
    if (len == 0) {
        s_radio.startReceive();
        return 0;
    }

    // Read the raw LoRa frame
    uint8_t rx_tmp[512];
    if (len > sizeof(rx_tmp)) {
        Serial.print("Radio: RX oversize len=");
        Serial.println(len);
        s_radio.startReceive();
        return -1;
    }
    int state = s_radio.readData(rx_tmp, len);
    float rssi = s_radio.getRSSI();
    float snr  = s_radio.getSNR();

    // Re-enter RX immediately so we don't miss the second half of a split
    s_radio.startReceive();

    if (state != RADIOLIB_ERR_NONE || len < 2) return 0;

    // Parse RNode header
    uint8_t rnode_hdr = rx_tmp[0];
    uint8_t seq = (rnode_hdr >> 4) & 0x0F;
    bool is_split = (rnode_hdr & FLAG_SPLIT) != 0;
    uint8_t* payload = rx_tmp + 1;
    size_t payload_len = len - 1;

    Serial.print("Radio: RX ");
    Serial.print(payload_len);
    Serial.print("B  RSSI=");
    Serial.print(rssi, 1);
    Serial.print(" dBm  SNR=");
    Serial.print(snr, 1);
    Serial.print(" dB");
    if (is_split) {
        Serial.print("  SPLIT seq=");
        Serial.print(seq);
        Serial.print(s_split_seq == 0xFF ? " (first)" : " (second)");
    }

    if (is_split) {
        if (s_split_seq == 0xFF) {
            // First half — buffer it
            if (payload_len > sizeof(s_split_buf)) {
                Serial.println("  SPLIT first half too large, dropping");
                return 0;
            }
            memcpy(s_split_buf, payload, payload_len);
            s_split_len = payload_len;
            s_split_seq = seq;
            s_split_ms  = millis();
            Serial.println("  (buffered)");
            return 0;  // not ready yet
        }
        else if (seq == s_split_seq) {
            // Second half — join with buffered first half
            size_t total = s_split_len + payload_len;
            if (total > bufsize || total > MAX_PAYLOAD) {
                Serial.println("  SPLIT reassembled too large, dropping");
                s_split_seq = 0xFF;
                s_split_len = 0;
                return 0;
            }
            memcpy(buf, s_split_buf, s_split_len);
            memcpy(buf + s_split_len, payload, payload_len);
            Serial.print("  (reassembled ");
            Serial.print(total);
            Serial.println("B)");
            s_split_seq = 0xFF;
            s_split_len = 0;

            // Log reassembled packet type
            if (total > 0) {
                uint8_t pt = buf[0] & 0x03;
                const char* pt_name = "UNK";
                switch (pt) { case 0: pt_name="DATA"; break; case 1: pt_name="ANNOUNCE"; break; case 2: pt_name="LINKREQ"; break; case 3: pt_name="PROOF"; break; }
                Serial.print("Radio: reassembled pt=");
                Serial.print(pt_name);
                Serial.print("  total=");
                Serial.print(total);
                Serial.println("B");
            }
            return (int)total;
        }
        else {
            // Different sequence — replace with new first half
            Serial.println("  SPLIT seq mismatch, replacing");
            memcpy(s_split_buf, payload, payload_len);
            s_split_len = payload_len;
            s_split_seq = seq;
            s_split_ms  = millis();
            return 0;
        }
    }

    // Non-split packet — deliver directly
    if (payload_len > bufsize) payload_len = bufsize;
    memcpy(buf, payload, payload_len);

    // Log packet type
    if (payload_len > 0) {
        uint8_t pt = buf[0] & 0x03;
        uint8_t ctx = (buf[0] >> 5) & 0x01;
        const char* pt_name = "UNK";
        switch (pt) { case 0: pt_name="DATA"; break; case 1: pt_name="ANNOUNCE"; break; case 2: pt_name="LINKREQ"; break; case 3: pt_name="PROOF"; break; }
        Serial.print("  pt=");
        Serial.print(pt_name);
        Serial.print("  ctx=");
        Serial.println(ctx);
    } else {
        Serial.println();
    }

    return (int)payload_len;
}

// Internal: force standby with recovery if SPI fails
static void _ensure_standby() {
    int state = s_radio.standby();
    if (state != RADIOLIB_ERR_NONE) {
        Serial.print("Radio: standby failed (");
        Serial.print(state);
        Serial.println("), resetting radio");
        digitalWrite(PIN_LORA_RESET, LOW);
        delay(10);
        digitalWrite(PIN_LORA_RESET, HIGH);
        delay(20);
        if (s_cfg_ptr) {
            begin(*s_cfg_ptr);
            s_radio.setPacketReceivedAction(isr_packet_received);
        }
    }
}

// Internal: transmit a single LoRa frame (header + payload)
static int _tx_frame(uint8_t header, const uint8_t* payload, size_t len) {
    uint8_t tx_buf[256];
    if (len + 1 > sizeof(tx_buf)) return -1;
    tx_buf[0] = header;
    memcpy(tx_buf + 1, payload, len);

    int state = s_radio.transmit(tx_buf, len + 1);
    if (state != RADIOLIB_ERR_NONE) {
        Serial.print("Radio: TX error code=");
        Serial.println(state);
    }
    return state;
}

int transmit(const uint8_t* buf, size_t len) {
    if (!s_online) return -1;
    if (len > MAX_PAYLOAD) return -1;

    // RNode-compatible framing with split support.
    // Single frame: header (random seq, no flags) + up to 254 payload bytes
    // Split frame:  two frames with same header (random seq + FLAG_SPLIT)
    //   Frame 1: header + first 254 bytes
    //   Frame 2: header + remaining bytes

    _ensure_standby();

    uint8_t header = (uint8_t)(random(256) & 0xF0);  // random seq, no flags
    int state;

    if (len <= SINGLE_MTU - 1) {
        // Single frame
        state = _tx_frame(header, buf, len);
    } else {
        // Split into two frames
        header |= FLAG_SPLIT;
        size_t first_len = SINGLE_MTU - 1;  // 254 bytes
        size_t second_len = len - first_len;

        Serial.print("Radio: TX split ");
        Serial.print(first_len);
        Serial.print("+");
        Serial.print(second_len);
        Serial.print("B (total ");
        Serial.print(len);
        Serial.println("B)");

        state = _tx_frame(header, buf, first_len);
        if (state == RADIOLIB_ERR_NONE) {
            // Brief pause between frames — matches RNode firmware behavior
            s_radio.startReceive();  // transition TX→RX→TX
            delay(1);
            _ensure_standby();
            state = _tx_frame(header, buf + first_len, second_len);
        }
    }

    s_radio.startReceive();
    // Clear the ISR flag AFTER re-entering RX. The SX1262's DIO1
    // line can glitch during the TX→Standby→RX transition, causing
    // a spurious "packet received" IRQ. Without this clear, the
    // next read_pending() call sees s_rx_flag=true, reads len=0
    // from getPacketLength() (no real packet), and wastes a cycle.
    // Worse, the spurious IRQ might leave the chip's IRQ status
    // register in a half-cleared state that delays or blocks the
    // next real RX_DONE edge. Clearing here is safe because any
    // real packet arriving after startReceive() will re-set the
    // flag via the ISR.
    s_rx_flag = false;
    return (state == RADIOLIB_ERR_NONE) ? (int)len : -1;
}

}} // namespace rlr::radio
