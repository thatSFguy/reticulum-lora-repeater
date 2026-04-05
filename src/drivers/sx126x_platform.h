#pragma once
// =====================================================================
//  src/drivers/sx126x_platform.h
//  ------------------------------------------------------------------
//  Compatibility shim that translates this project's board-header
//  vocabulary (include/board/<name>.h) into the legacy upstream RNode
//  firmware's `Boards.h` vocabulary that `sx126x.cpp` expects.
//
//  The sx126x driver itself is ported almost verbatim from the sibling
//  project `microReticulum_Faketec_Repeater` to keep close-to-upstream
//  compatibility for future syncs. Rather than mass-rewriting the
//  driver to use our new macro names, we define the legacy names here
//  in terms of the new ones. Future per-board changes go in
//  include/board/<name>.h and this shim stays stable.
// =====================================================================

#include <Arduino.h>

// The board header (Faketec.h, etc.) is pre-included by platformio.ini
// via the `-include` compiler flag, so all PIN_LORA_*, HAS_*, and
// RADIO_* macros from it are already in scope when this file is
// preprocessed. No #include needed here.

// ---- Modem type constants (from legacy Modem.h) -------------------
#define SX1276 0x01
#define SX1278 0x02
#define SX1262 0x03
#define SX1280 0x04

// This project only builds SX1262 for now.
#ifndef MODEM
  #define MODEM SX1262
#endif

// ---- Platform / MCU indicators (from legacy Boards.h) -------------
// nRF52 only — all ESP32 / AVR branches in sx126x.cpp are dead code
// from our perspective, preserved for upstream compatibility.
#define PLATFORM_AVR      1
#define PLATFORM_ESP32    2
#define PLATFORM_NRF52    3

#define MCU_1284P         0x91
#define MCU_2560          0x92
#define MCU_ESP32         0x81
#define MCU_NRF52         0x72

#ifndef PLATFORM
  #define PLATFORM PLATFORM_NRF52
#endif
#ifndef MCU_VARIANT
  #define MCU_VARIANT MCU_NRF52
#endif

// ---- Board ID constants used in the driver's legacy #if chains ----
// Only Faketec is defined here — all other BOARD_* IDs the driver
// references are left undefined, so their #if branches evaluate to
// "BOARD_MODEL == 0" which is always false for our BOARD_Faketec
// build. The compiler silently dead-codes them.
//
// When adding a new board to this project, either:
//   (a) keep RADIO_TCXO_VOLTAGE_MV + RADIO_SPI_OVERRIDE_PINS in the
//       new board's header and the macro-driven arms in sx126x.cpp
//       handle it automatically — no changes needed here, OR
//   (b) if the new hardware needs a driver quirk that doesn't fit
//       the macro model, add its BOARD_<name> here.
#ifndef BOARD_Faketec
  #define BOARD_Faketec 0x52
#endif

// ---- Legacy pin_* globals the driver references ------------------
// sx126x.cpp uses these inside its SPI.begin() / SPI.setPins() calls.
// They're constexpr here so multiple translation units get consistent
// values without linker conflict, and they map directly to the board
// header's PIN_LORA_* macros.
static constexpr int pin_cs    = PIN_LORA_NSS;
static constexpr int pin_sclk  = PIN_LORA_SCK;
static constexpr int pin_mosi  = PIN_LORA_MOSI;
static constexpr int pin_miso  = PIN_LORA_MISO;
static constexpr int pin_reset = PIN_LORA_RESET;
static constexpr int pin_busy  = PIN_LORA_BUSY;
static constexpr int pin_dio   = PIN_LORA_DIO1;
static constexpr int pin_rxen  = PIN_LORA_RXEN;
static constexpr int pin_txen  = PIN_LORA_TXEN;
static constexpr int pin_tcxo_enable = -1;  // TCXO powered from SX1262 DIO3 on all supported boards

// ---- Feature flags the driver checks -----------------------------
#if RADIO_DIO2_AS_RF_SWITCH
  #define DIO2_AS_RF_SWITCH true
#else
  #define DIO2_AS_RF_SWITCH false
#endif

// Over-current protection trim value for the SX1262. The sibling
// project's Boards.h defines this per-board and falls back to 0x18
// globally when no board override is set. 0x18 is the SX1262
// datasheet's recommended value for +22 dBm operation with the
// PA boost path.
#ifndef OCP_TUNED
  #define OCP_TUNED 0x18
#endif

// These are not used by any currently-supported board but the driver
// references them. Leave undefined so #if branches compile to false.
// #define HAS_LORA_PA
// #define HAS_LORA_LNA
// #define HAS_NOTUSINGINTERRUPT

// ISR attribute macro — empty on nRF52 (only ESP32's IRAM_ATTR uses it)
#ifndef ISR_VECT
  #define ISR_VECT
#endif
