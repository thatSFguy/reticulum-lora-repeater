#pragma once
// =====================================================================
//  include/board/xiao_nrf52840.h
//  ------------------------------------------------------------------
//  Seeed XIAO nRF52840 Kit — XIAO nRF52840 MCU board + Wio-SX1262
//  LoRa daughter board (the kit Seeed sells as a bundle).
//
//  The Wio-SX1262 daughter board carries an SX1262 with TCXO, an
//  external LNA gated by RXEN (same topology as the Ebyte E22 on the
//  Faketec), and DIO2-as-RF-switch for the TX path. No VEXT gate —
//  the radio runs directly off the XIAO's 3V3 rail.
//
//  Pin values below are the pca10056 Arduino pin numbers (P0.x == x,
//  P1.x == 32+x) derived from Meshtastic's
//  variants/nrf52840/seeed_xiao_nrf52840_kit/variant.h + variant.cpp.
//  The Meshtastic variant uses XIAO-specific Arduino pin numbers (D0-D10)
//  which map to physical nRF52840 GPIOs via g_ADigitalPinMap; the
//  values here are the physical GPIOs translated to pca10056 convention
//  so we can use board = nrf52840_dk_adafruit consistently across all
//  our board targets.
//
//  Cross-checked against:
//    * Meshtastic firmware variant (commit 9322bcdb)
//    * Seeed XIAO nRF52840 pinout diagram
//    * Wio-SX1262 for XIAO schematic
// =====================================================================

// ---- Board identity ------------------------------------------------
#define BOARD_NAME              "XIAO nRF52840 Kit"
#define BOARD_MANUFACTURER      "Seeed Studio"
#define BOARD_XIAO_NRF52840     0x53
#define PRODUCT_XIAO_NRF52840   0x19
#define MODEL_XIAO_NRF52840     0x19

// ---- Capability flags ----------------------------------------------
#define HAS_TCXO                1
#define HAS_RF_SWITCH_RX_TX     1      // Wio-SX1262 has external LNA gated by RXEN
#define HAS_BUSY                1
#define HAS_LED                 1      // XIAO has RGB LED (3 pins); we use blue for heartbeat
#define HAS_BUTTON              0      // no user button, only reset
#define HAS_BATTERY_SENSE       1      // P0.31 via voltage divider
#define HAS_VEXT_RAIL           0      // no GPIO-gated power rail; radio is always on
#define HAS_DISPLAY             0
#define HAS_BLE                 0      // intentionally disabled in v0.1
#define HAS_PMU                 0

// ---- MCU / SRAM budget --------------------------------------------
#define BOARD_MCU               "nRF52840"
#define BOARD_SRAM_BYTES        262144    // 256 KB total
#define BOARD_FLASH_BYTES       1048576   // 1 MB total (app gets ~800 KB)

// ---- Radio module --------------------------------------------------
#define RADIO_CHIP              "SX1262"
#define RADIO_MODULE            "Wio-SX1262 for XIAO"
#define RADIO_TCXO_VOLTAGE_MV   1800       // 1.8V TCXO, same as E22
#define RADIO_SPI_OVERRIDE_PINS 1          // must remap SPI to Wio daughter board pins
#define RADIO_DIO2_AS_RF_SWITCH 1
#define RADIO_MAX_DBM           22         // SX1262 core max

// ---- Pin numbers (pca10056 convention) -----------------------------
//
// Meshtastic variant.h  →  XIAO Dx  →  nRF52840 Px.xx  →  pca10056 pin
//   SX126X_CS    = D4   →  P0.04   →  4
//   SX126X_DIO1  = D1   →  P0.03   →  3
//   SX126X_BUSY  = D3   →  P0.29   →  29
//   SX126X_RESET = D2   →  P0.28   →  28
//   SX126X_RXEN  = D5   →  P0.05   →  5
//   SPI SCK      = D8   →  P1.13   →  45
//   SPI MOSI     = D10  →  P1.15   →  47
//   SPI MISO     = D9   →  P1.14   →  46
//
#define PIN_LORA_NSS            4     // P0.04 — CS
#define PIN_LORA_SCK            45    // P1.13
#define PIN_LORA_MOSI           47    // P1.15
#define PIN_LORA_MISO           46    // P1.14
#define PIN_LORA_RESET          28    // P0.28
#define PIN_LORA_BUSY           29    // P0.29
#define PIN_LORA_DIO1           3     // P0.03 — IRQ line
#define PIN_LORA_RXEN           5     // P0.05 — external LNA enable (same as Faketec topology)
#define PIN_LORA_TXEN           -1    // handled by DIO2_AS_RF_SWITCH

// Power / peripherals
// No VEXT gate on the XIAO kit — radio runs directly off 3V3.
// PIN_VEXT_EN intentionally not defined (HAS_VEXT_RAIL = 0).

// Battery sense — P0.31 via voltage divider on the XIAO board.
// The Sense variant has a built-in divider; the plain XIAO may not.
// batt_mult default is a rough guess; user calibrates via webflasher.
#define PIN_BATTERY             31    // P0.31
#define BATTERY_ADC_RESOLUTION  12

// LED — XIAO nRF52840 has an RGB LED (active LOW, common anode):
//   Red:   P0.26 (pin 26)
//   Blue:  P0.06 (pin 6)
//   Green: P0.30 (pin 30)
// Using blue (most visible) as the heartbeat LED.
#define PIN_LED                 6     // P0.06 — blue LED
#define LED_ACTIVE_HIGH         0     // XIAO LEDs are ACTIVE LOW

// ---- Default config values for first boot -------------------------
#define DEFAULT_CONFIG_FREQ_HZ          915000000UL
#define DEFAULT_CONFIG_BW_HZ            125000UL
#define DEFAULT_CONFIG_SF               10
#define DEFAULT_CONFIG_CR               5
#define DEFAULT_CONFIG_TXP_DBM          14
#define DEFAULT_CONFIG_BATT_MULT        1.0f    // calibrate via CALIBRATE BATTERY <mv>
#define DEFAULT_CONFIG_DISPLAY_NAME     "XIAO Repeater"
