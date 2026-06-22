#pragma once
// =====================================================================
//  include/board/techo.h
//  ------------------------------------------------------------------
//  LilyGO T-Echo — Nordic nRF52840 + Semtech SX1262 (+ GPS, e-paper,
//  touch button). The SX1262 has an integrated TCXO and uses DIO2 as
//  the internal RF switch — no external RXEN/TXEN line, same topology
//  as the RAK4631 and Heltec T114.
//
//  Pin values are mined from:
//    * Meshtastic firmware variant: variants/nrf52840/t-echo/variant.h
//    * markqvist/RNode_Firmware Boards.h (identity codes)
//
//  Using the pca10056 Arduino pin numbering (platformio.ini sets
//  board = nrf52840_dk_adafruit):
//    P0.x == x        for x in 0..31
//    P1.x == 32 + x   for x in 0..15
//
//  The T-Echo ships with the Adafruit nRF52 bootloader + SoftDevice
//  S140 v6.1.1 — identical to nrf52840_dk_adafruit — so it needs NO
//  custom board JSON or linker script (unlike the XIAO, which ships
//  S140 v7.3.0). The SX1262 LoRa SPI pinout is byte-for-byte identical
//  to the Heltec T114; only the power/LED/battery wiring differs.
//
//  HARDWARE VALIDATION PENDING: no T-Echo was present on the bench when
//  this header landed. Pins are sourced from Meshtastic, not bench-
//  verified. First user to flash should confirm RX works end-to-end
//  and recalibrate batt_mult against a multimeter via the serial
//  console / webflasher CALIBRATE BATTERY flow.
// =====================================================================

// ---- Board identity ------------------------------------------------
// Codes match markqvist/RNode_Firmware Boards.h. Decorative here — only
// BOARD_NAME is printed at boot; PRODUCT/MODEL are not referenced.
#define BOARD_NAME              "T-Echo"
#define BOARD_MANUFACTURER      "LilyGO"
#define BOARD_TECHO             0x44
#define PRODUCT_TECHO           0x15
#define MODEL_TECHO             0x17   // 868/915 MHz variant

// ---- Capability flags ----------------------------------------------
#define HAS_TCXO                1
#define HAS_RF_SWITCH_RX_TX     1      // DIO2 handles TX/RX switching (internal)
#define HAS_BUSY                1
#define HAS_LED                 1
#define HAS_BUTTON              1      // touch button on P0.11 + buttons P1.10/P0.18
#define HAS_BATTERY_SENSE       1
#define HAS_VEXT_RAIL           1      // PIN_POWER_EN (P0.12) gates LoRa+GPS+eink rail
#define HAS_DISPLAY             0      // hardware has e-paper, firmware doesn't use it
#define HAS_BLE                 1
#define HAS_PMU                 0

// ---- MCU / SRAM budget --------------------------------------------
#define BOARD_MCU               "nRF52840"
#define BOARD_SRAM_BYTES        262144    // 256 KB total
#define BOARD_FLASH_BYTES       1048576   // 1 MB total (app gets ~800 KB)

// ---- Radio module --------------------------------------------------
#define RADIO_CHIP              "SX1262"
#define RADIO_MODULE            "T-Echo integrated"
// Meshtastic variant sets SX126X_DIO3_TCXO_VOLTAGE 1.8.
#define RADIO_TCXO_VOLTAGE_MV   1800
#define RADIO_SPI_OVERRIDE_PINS 1
#define RADIO_DIO2_AS_RF_SWITCH 1
#define RADIO_MAX_DBM           22        // SX1262 core max

// ---- Pin numbers (pca10056 convention, 1:1 mapping) ----------------
// LoRa control lines — identical to the Heltec T114 layout.
#define PIN_LORA_NSS            24    // P0.24
#define PIN_LORA_SCK            19    // P0.19
#define PIN_LORA_MOSI           22    // P0.22
#define PIN_LORA_MISO           23    // P0.23
#define PIN_LORA_RESET          25    // P0.25
#define PIN_LORA_BUSY           17    // P0.17
#define PIN_LORA_DIO1           20    // P0.20

// T-Echo wires the SX1262 DIO2 to an internal analog switch for TX/RX
// switching — no external LNA enable line. -1 signals "not present" to
// the Radio.cpp init code.
#define PIN_LORA_RXEN           -1
#define PIN_LORA_TXEN           -1

// Power — PIN_POWER_EN (P0.12) gates the shared 3V3 rail feeding the
// LoRa radio, GPS and e-paper. ACTIVE HIGH; Radio.cpp drives it HIGH at
// boot and waits VEXT_SETTLE_MS before talking to the radio.
#define PIN_VEXT_EN             12    // P0.12
#define VEXT_SETTLE_MS          10

// Battery sense. The T-Echo wires a 1:1 divider from the battery rail
// into P0.04 (A0); the on-air voltage is the ADC reading times 2.
// Meshtastic reads it against the 3.0 V internal reference, so the raw
// 12-bit reading maps to mV via roughly (adc / 4095) * 3000 * 2 =
// adc * 1.465 mV/LSB. Used as the first-boot default for
// DEFAULT_CONFIG_BATT_MULT below; CALIBRATE BATTERY refines per-device.
#define PIN_BATTERY             4     // P0.04
#define BATTERY_ADC_RESOLUTION  12

// LED — T-Echo's LEDs are active LOW (Meshtastic LED_STATE_ON == 0).
// Green LED on P0.13.
#define PIN_LED                 13    // P0.13
#define LED_ACTIVE_HIGH         0

// Button — touch button on P0.11
#define PIN_BUTTON              11

// ---- Default config values for first boot -------------------------
// US ISM band with the SX1262 core at its max — the webflasher's
// CONFIG SET / COMMIT flow lets the user retune for their region (the
// T-Echo also ships in a 433 MHz variant). batt_mult is a first guess
// for the 1:1 divider against the 3.0 V reference — user runs
// CALIBRATE BATTERY <measured_mv> on first boot.
#define DEFAULT_CONFIG_FREQ_HZ          915000000UL
#define DEFAULT_CONFIG_BW_HZ            125000UL
#define DEFAULT_CONFIG_SF               10
#define DEFAULT_CONFIG_CR               5
#define DEFAULT_CONFIG_TXP_DBM          22
#define DEFAULT_CONFIG_BATT_MULT        1.465f
#define DEFAULT_CONFIG_DISPLAY_NAME     "Rptr-T-Echo"
