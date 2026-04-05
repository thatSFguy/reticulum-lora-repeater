#pragma once
// =====================================================================
//  include/board/Faketec.h
//  ------------------------------------------------------------------
//  Nice!Nano-style nRF52840 ProMicro clone + Ebyte E22-900M30S
//  (SX1262, TCXO, external RF switch, ~30 dBm via external PA).
//
//  ALL Faketec-specific constants live in this file. Nothing in
//  src/ ever uses `#if BOARD_MODEL == BOARD_Faketec`. platformio.ini
//  pre-includes this header for every compilation unit in the
//  [env:Faketec] build via `-include include/board/Faketec.h`.
//
//  Authoritative pin map source: Meshtastic DIY variant
//  `nrf52840/diy/nrf52_promicro_diy_tcxo/variant.h`. Cross-verified
//  against MeshCore `variants/promicro/target.h`. Do not change the
//  values below without a multimeter trace + a test on real hardware.
// =====================================================================

// ---- Board identity ------------------------------------------------
#define BOARD_NAME              "Faketec"
#define BOARD_MANUFACTURER      "DIY / Nice!Nano clone"
#define BOARD_Faketec           0x52
#define PRODUCT_Faketec         0x18
#define MODEL_Faketec           0x18

// ---- Capability flags ----------------------------------------------
#define HAS_TCXO                1
#define HAS_RF_SWITCH_RX_TX     1
#define HAS_BUSY                1
#define HAS_LED                 1
#define HAS_BUTTON              1
#define HAS_BATTERY_SENSE       1
#define HAS_VEXT_RAIL           1      // PIN_VEXT_EN gates radio 3V3
#define HAS_DISPLAY             0
#define HAS_BLE                 0      // deliberately disabled in v0.1
#define HAS_PMU                 0

// ---- MCU / SRAM budget --------------------------------------------
#define BOARD_MCU               "nRF52840"
#define BOARD_SRAM_BYTES        262144     // 256 KB total
#define BOARD_FLASH_BYTES       1048576    // 1 MB total (app gets ~800 KB)

// ---- Radio module --------------------------------------------------
#define RADIO_CHIP              "SX1262"
#define RADIO_MODULE            "Ebyte E22-900M30S"
#define RADIO_TCXO_VOLTAGE_MV   1800       // MeshCore PromicroBoard.h; E22 uses 1.8V
#define RADIO_SPI_OVERRIDE_PINS 1          // pca10056 variant defaults collide; setPins needed
#define RADIO_DIO2_AS_RF_SWITCH 1
#define RADIO_MAX_DBM           22         // SX1262 core max; ext PA adds ~8 dB

// ---- Pin numbers ---------------------------------------------------
// nRF52 Arduino pin convention used by the pca10056 variant:
//   P0.x == x  (for x in 0..31)
//   P1.x == 32 + x  (for x in 0..15)
//
// Radio (confirmed against Meshtastic DIY + MeshCore references)
#define PIN_LORA_NSS            45    // P1.13
#define PIN_LORA_SCK            43    // P1.11
#define PIN_LORA_MOSI           47    // P1.15
#define PIN_LORA_MISO            2    // P0.02
#define PIN_LORA_RESET           9    // P0.09
#define PIN_LORA_BUSY           29    // P0.29
#define PIN_LORA_DIO1           10    // P0.10  (IRQ line for RX done etc.)
#define PIN_LORA_RXEN           17    // P0.17  (external LNA enable)
#define PIN_LORA_TXEN           -1    // handled by DIO2_AS_RF_SWITCH

// Power / peripherals
#define PIN_VEXT_EN             13    // P0.13  ACTIVE HIGH — gates 3V3 rail to radio
#define VEXT_SETTLE_MS          10    // settle time after asserting VEXT

#define PIN_BATTERY             31    // P0.31  ADC input for battery sense
#define BATTERY_ADC_RESOLUTION  12    // 12-bit analogReadResolution

// LED — single user LED on P0.15 (PIN_LED1 on Nice!Nano variants)
#define PIN_LED                 15    // P0.15
#define LED_ACTIVE_HIGH         1

// Button
#define PIN_BUTTON              32    // P1.00

// ---- Default config values for first boot -------------------------
// These are what flashes into /config_store_0.dat the very first time
// the firmware boots on a freshly-flashed Faketec. The user then
// overrides via the webflasher or serial console. Pick safe,
// universally-legal defaults here — the webflasher will force region
// selection before the user actually commits.
#define DEFAULT_CONFIG_FREQ_HZ          915000000UL  // US ISM center; webflasher reconciles
#define DEFAULT_CONFIG_BW_HZ            125000UL     // narrow/long-range
#define DEFAULT_CONFIG_SF               10
#define DEFAULT_CONFIG_CR               5
#define DEFAULT_CONFIG_TXP_DBM          14           // conservative until user sets
#define DEFAULT_CONFIG_BATT_MULT        1.284f       // field-calibrated for first unit
#define DEFAULT_CONFIG_DISPLAY_NAME     "Unconfigured Repeater"
