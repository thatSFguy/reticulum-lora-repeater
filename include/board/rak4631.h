#pragma once
// =====================================================================
//  include/board/rak4631.h
//  ------------------------------------------------------------------
//  RAK4631 / WisBlock Core — nRF52840 + SX1262
//
//  NOT YET IMPLEMENTED in v0.1. This file is a stub that exists so
//  the one-header-per-board pattern is provable and so adding RAK
//  support in v0.2 is a matter of filling in the values below, not
//  restructuring the build.
//
//  To activate this board:
//    1. Populate every field below from the RAK4631 datasheet +
//       MeshCore variants/rak4631/ reference
//    2. Delete this #error
//    3. Uncomment the [env:rak4631] block in platformio.ini
//    4. Build and test
//  -----------------------------------------------------------------

#error "rak4631 board support is not yet implemented — see comments in include/board/rak4631.h"

// ---- Board identity ------------------------------------------------
#define BOARD_NAME              "RAK4631"
#define BOARD_MANUFACTURER      "RAKwireless"
#define BOARD_RAK4631           0x51
#define PRODUCT_RAK4631         0x10
#define MODEL_RAK4631           0x12

// ---- Capability flags ----------------------------------------------
#define HAS_TCXO                1
#define HAS_RF_SWITCH_RX_TX     1
#define HAS_BUSY                1
#define HAS_LED                 1
#define HAS_BUTTON              0      // WisBlock Core alone has no user button
#define HAS_BATTERY_SENSE       1
#define HAS_VEXT_RAIL           0      // RAK4631 powers radio directly, no GPIO gate
#define HAS_DISPLAY             0
#define HAS_BLE                 0
#define HAS_PMU                 0

// ---- MCU / SRAM budget --------------------------------------------
#define BOARD_MCU               "nRF52840"
#define BOARD_SRAM_BYTES        262144
#define BOARD_FLASH_BYTES       1048576

// ---- Radio module --------------------------------------------------
#define RADIO_CHIP              "SX1262"
#define RADIO_MODULE            "RAK4631 integrated"
#define RADIO_TCXO_VOLTAGE_MV   3300       // RAK4631 uses 3.3V TCXO (different from E22)
#define RADIO_SPI_OVERRIDE_PINS 0          // RAK4631 uses default SPI pins, no override
#define RADIO_DIO2_AS_RF_SWITCH 1
#define RADIO_MAX_DBM           22

// ---- Pin numbers ---------------------------------------------------
// TODO: fill in from wiscore_rak4631 variant / RAK datasheet
#define PIN_LORA_NSS            42
#define PIN_LORA_SCK            43
#define PIN_LORA_MOSI           44
#define PIN_LORA_MISO           45
#define PIN_LORA_RESET          38
#define PIN_LORA_BUSY           46
#define PIN_LORA_DIO1           47
#define PIN_LORA_RXEN           37
#define PIN_LORA_TXEN           -1

#define PIN_BATTERY             -1    // TODO: verify against RAK4631 schematic
#define BATTERY_ADC_RESOLUTION  12

#define PIN_LED                 -1    // TODO: PIN_LED1 / PIN_LED2 on RAK4631
#define LED_ACTIVE_HIGH         1

// ---- Default config values for first boot -------------------------
#define DEFAULT_CONFIG_FREQ_HZ          915000000UL
#define DEFAULT_CONFIG_BW_HZ            125000UL
#define DEFAULT_CONFIG_SF               10
#define DEFAULT_CONFIG_CR               5
#define DEFAULT_CONFIG_TXP_DBM          14
#define DEFAULT_CONFIG_BATT_MULT        1.0f   // TODO: calibrate
#define DEFAULT_CONFIG_DISPLAY_NAME     "Unconfigured Repeater"
