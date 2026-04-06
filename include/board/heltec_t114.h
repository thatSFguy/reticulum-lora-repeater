#pragma once
// =====================================================================
//  include/board/heltec_t114.h
//  ------------------------------------------------------------------
//  Heltec Mesh Node T114 — nRF52840 + SX1262 + 1.14" TFT display
//
//  Pin values from Meshtastic firmware variant
//  variants/nrf52840/heltec_mesh_node_t114 (commit 9322bcdb).
//  The variant.cpp uses direct 1:1 pin mapping (Arduino pin N =
//  nRF52840 P0.N / P1.(N-32)), same as pca10056.
//
//  Key features:
//    * Integrated SX1262 with TCXO (1.8V) and DIO2-as-RF-switch
//    * No external RXEN/TXEN — DIO2 handles TX/RX switching alone
//    * VEXT_ENABLE (P0.21) gates external peripherals + radio 3V3
//    * ADC_CTRL (P0.06) gates the battery voltage divider
//    * Has a 1.14" TFT display (not used by our firmware)
//    * Has a physical button (P1.10 = pin 42)
// =====================================================================

// ---- Board identity ------------------------------------------------
#define BOARD_NAME              "Heltec T114"
#define BOARD_MANUFACTURER      "Heltec"
#define BOARD_HELTEC_T114       0x54
#define PRODUCT_HELTEC_T114     0x20
#define MODEL_HELTEC_T114       0x20

// ---- Capability flags ----------------------------------------------
#define HAS_TCXO                1
#define HAS_RF_SWITCH_RX_TX     1      // DIO2 handles TX/RX switching (no external RXEN)
#define HAS_BUSY                1
#define HAS_LED                 1
#define HAS_BUTTON              1      // P1.10
#define HAS_BATTERY_SENSE       1
#define HAS_VEXT_RAIL           1      // VEXT_ENABLE on P0.21
#define HAS_DISPLAY             0      // hardware has TFT, firmware doesn't use it
#define HAS_BLE                 0
#define HAS_PMU                 0

// ---- MCU / SRAM budget --------------------------------------------
#define BOARD_MCU               "nRF52840"
#define BOARD_SRAM_BYTES        262144
#define BOARD_FLASH_BYTES       1048576

// ---- Radio module --------------------------------------------------
#define RADIO_CHIP              "SX1262"
#define RADIO_MODULE            "Heltec T114 integrated"
#define RADIO_TCXO_VOLTAGE_MV   1800
#define RADIO_SPI_OVERRIDE_PINS 1
#define RADIO_DIO2_AS_RF_SWITCH 1
#define RADIO_MAX_DBM           22

// ---- Pin numbers (pca10056 convention, 1:1 mapping) ----------------
#define PIN_LORA_NSS            24    // P0.24
#define PIN_LORA_SCK            19    // P0.19
#define PIN_LORA_MOSI           22    // P0.22
#define PIN_LORA_MISO           23    // P0.23
#define PIN_LORA_RESET          25    // P0.25
#define PIN_LORA_BUSY           17    // P0.17
#define PIN_LORA_DIO1           20    // P0.20
#define PIN_LORA_RXEN           -1    // no external LNA — DIO2 handles everything
#define PIN_LORA_TXEN           -1

// Power
#define PIN_VEXT_EN             21    // P0.21 — gates external peripherals + radio 3V3
#define VEXT_SETTLE_MS          10

// Battery — P0.04 via gated voltage divider (ADC_CTRL on P0.06)
// Note: ADC_CTRL needs to be driven HIGH to enable the divider
// before reading. Our firmware doesn't handle this yet — battery
// readings may be zero until ADC_CTRL support is added. Calibrate
// via CALIBRATE BATTERY once it's working.
#define PIN_BATTERY             4     // P0.04
#define BATTERY_ADC_RESOLUTION  12

// LED — green LED on P1.03 (pin 35)
#define PIN_LED                 35    // P1.03
#define LED_ACTIVE_HIGH         1

// Button — P1.10 (pin 42)
#define PIN_BUTTON              42

// ---- Default config values for first boot -------------------------
#define DEFAULT_CONFIG_FREQ_HZ          915000000UL
#define DEFAULT_CONFIG_BW_HZ            125000UL
#define DEFAULT_CONFIG_SF               10
#define DEFAULT_CONFIG_CR               5
#define DEFAULT_CONFIG_TXP_DBM          14
#define DEFAULT_CONFIG_BATT_MULT        1.0f
#define DEFAULT_CONFIG_DISPLAY_NAME     "Heltec T114 Repeater"
