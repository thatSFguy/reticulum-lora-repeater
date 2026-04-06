#pragma once
// =====================================================================
//  include/board/rak4631.h
//  ------------------------------------------------------------------
//  RAK4631 / WisBlock Core — Nordic nRF52840 + Semtech SX1262.
//  Integrated TCXO, integrated RF switch driven by DIO2 (no external
//  LNA enable line — simpler than the Faketec/E22 topology).
//
//  Pin values below are mined from:
//    * Meshtastic firmware variant: variants/nrf52840/rak4631/variant.h
//    * RAKwireless WisCore RAK4631 hardware reference
//    * MeshCore CustomSX1262.h for the RAK wrapper
//
//  Cross-checked against all three and using the pca10056 Arduino pin
//  numbering (our platformio.ini sets board = nrf52840_dk_adafruit):
//    P0.x == x        for x in 0..31
//    P1.x == 32 + x   for x in 0..15
//
//  HARDWARE VALIDATION PENDING: no RAK4631 was present on the bench
//  when this header landed. First user to flash should verify RX
//  works end-to-end and recalibrate batt_mult against a multimeter
//  via the serial console / webflasher.
// =====================================================================

// ---- Board identity ------------------------------------------------
#define BOARD_NAME              "RAK4631"
#define BOARD_MANUFACTURER      "RAKwireless"
#define BOARD_RAK4631           0x51
#define PRODUCT_RAK4631         0x10
#define MODEL_RAK4631           0x12

// ---- Capability flags ----------------------------------------------
#define HAS_TCXO                1
#define HAS_RF_SWITCH_RX_TX     1      // DIO2 handles TX/RX switching (internal)
#define HAS_BUSY                1
#define HAS_LED                 1
#define HAS_BUTTON              0      // WisBlock Core alone has no user button
#define HAS_BATTERY_SENSE       1
#define HAS_VEXT_RAIL           1      // PIN_VEXT_EN (P1.05) gates the SX1262 3V3 rail
#define HAS_DISPLAY             0
#define HAS_BLE                 0      // intentionally disabled in v0.1
#define HAS_PMU                 0

// ---- MCU / SRAM budget --------------------------------------------
#define BOARD_MCU               "nRF52840"
#define BOARD_SRAM_BYTES        262144    // 256 KB total
#define BOARD_FLASH_BYTES       1048576   // 1 MB total (app gets ~800 KB)

// ---- Radio module --------------------------------------------------
#define RADIO_CHIP              "SX1262"
#define RADIO_MODULE            "RAK4631 integrated"
// Meshtastic variant.h sets SX126X_DIO3_TCXO_VOLTAGE 1.8, same as E22.
#define RADIO_TCXO_VOLTAGE_MV   1800
// The nRF52840's SPI peripherals are fully pin-remappable via the
// PSEL registers, so RADIO_SPI_OVERRIDE_PINS=1 + calling
// SPI.setPins(MISO, SCK, MOSI) at init is enough to talk to the
// SX1262 without switching to the SPI1 object the way Meshtastic's
// variant does.
#define RADIO_SPI_OVERRIDE_PINS 1
#define RADIO_DIO2_AS_RF_SWITCH 1
#define RADIO_MAX_DBM           22        // SX1262 core max

// ---- Pin numbers ---------------------------------------------------
// LoRa control lines — these match both the Meshtastic variant (in
// pca10056 Arduino numbering) and the RAK4631 hardware schematic.
#define PIN_LORA_NSS            42    // P1.10
#define PIN_LORA_SCK            43    // P1.11
#define PIN_LORA_MOSI           44    // P1.12
#define PIN_LORA_MISO           45    // P1.13
#define PIN_LORA_RESET          38    // P1.06
#define PIN_LORA_BUSY           46    // P1.14
#define PIN_LORA_DIO1           47    // P1.15  (IRQ line)

// RAK4631 does NOT expose an external LNA enable line — DIO2 handles
// both TX and RX switching on the integrated RF front-end. This is
// the simpler case compared to Faketec/E22 where PIN_LORA_RXEN is a
// real GPIO and must be fed to setRfSwitchPins(). -1 signals "not
// present" to the Radio.cpp init code.
#define PIN_LORA_RXEN           -1
#define PIN_LORA_TXEN           -1

// Power / peripherals
#define PIN_VEXT_EN             37    // P1.05  ACTIVE HIGH — gates SX1262 3V3
#define VEXT_SETTLE_MS          10

// Battery sense. The RAK4631 WisCore wires a 1:3 voltage divider
// from the battery rail into P0.04 (AIN2). AREF on this board is
// 3.0 V, so the raw 12-bit ADC reading maps to mV via roughly
// (adc / 4095) * 3000 * 3 = adc * 2.198 mV/LSB. Used as the
// first-boot default for DEFAULT_CONFIG_BATT_MULT below; the
// webflasher's CALIBRATE BATTERY flow will refine per-device.
#define PIN_BATTERY             4     // P0.04
#define BATTERY_ADC_RESOLUTION  12

// LED — RAK4631 Green LED1 on P1.03 (pin 35 in pca10056 numbering).
#define PIN_LED                 35
#define LED_ACTIVE_HIGH         1

// ---- Default config values for first boot -------------------------
// US ISM band with conservative TX power — the webflasher's CONFIG
// SET / COMMIT flow lets the user raise this after they've confirmed
// the regulatory region. Matches the bench defaults we use for the
// Faketec except for the TX power cap, which is lower here because
// the RAK4631 has no external PA and the SX1262 core is the final
// amplifier. batt_mult is a first guess for the 1:3 divider — user
// runs CALIBRATE BATTERY <measured_mv> on first boot.
#define DEFAULT_CONFIG_FREQ_HZ          915000000UL
#define DEFAULT_CONFIG_BW_HZ            125000UL
#define DEFAULT_CONFIG_SF               10
#define DEFAULT_CONFIG_CR               5
#define DEFAULT_CONFIG_TXP_DBM          22
#define DEFAULT_CONFIG_BATT_MULT        2.198f
#define DEFAULT_CONFIG_DISPLAY_NAME     "Rptr-RAK4631"
