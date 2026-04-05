#pragma once
// =====================================================================
//  include/board/xiao_nrf52840.h
//  ------------------------------------------------------------------
//  Seeed XIAO nRF52840 (Sense or plain) + external E22 radio module
//
//  NOT YET IMPLEMENTED in v0.1. Stub exists to prove the one-header-
//  per-board pattern. To activate:
//
//    1. Populate every field from the XIAO nRF52840 pinout + the
//       specific E22 wiring on the carrier board
//    2. Delete this #error
//    3. Uncomment the [env:xiao_nrf52840] block in platformio.ini
//    4. Build and test
//
//  Note: the XIAO has a much smaller pin count than the Nice!Nano,
//  so SPI + radio control + battery sense may force compromises.
//  -----------------------------------------------------------------

#error "xiao_nrf52840 board support is not yet implemented — see comments in include/board/xiao_nrf52840.h"

// ---- Board identity ------------------------------------------------
#define BOARD_NAME              "XIAO nRF52840"
#define BOARD_MANUFACTURER      "Seeed Studio"
#define BOARD_XIAO_NRF52840     0x53
#define PRODUCT_XIAO_NRF52840   0x19
#define MODEL_XIAO_NRF52840     0x19

// ---- Capability flags ----------------------------------------------
#define HAS_TCXO                1
#define HAS_RF_SWITCH_RX_TX     1
#define HAS_BUSY                1
#define HAS_LED                 1    // XIAO has built-in RGB
#define HAS_BUTTON              0    // no physical button, only reset
#define HAS_BATTERY_SENSE       1    // P0.31 via divider on Sense variant
#define HAS_VEXT_RAIL           0    // TODO: does the E22 carrier have a power gate?
#define HAS_DISPLAY             0
#define HAS_BLE                 0
#define HAS_PMU                 0

// ---- MCU / SRAM budget --------------------------------------------
#define BOARD_MCU               "nRF52840"
#define BOARD_SRAM_BYTES        262144
#define BOARD_FLASH_BYTES       1048576

// ---- Radio module --------------------------------------------------
#define RADIO_CHIP              "SX1262"
#define RADIO_MODULE            "Ebyte E22-900M30S (external)"
#define RADIO_TCXO_VOLTAGE_MV   1800      // E22 = 1.8V TCXO
#define RADIO_SPI_OVERRIDE_PINS 1         // XIAO default SPI likely won't match E22 wiring
#define RADIO_DIO2_AS_RF_SWITCH 1
#define RADIO_MAX_DBM           22

// ---- Pin numbers ---------------------------------------------------
// TODO: fill in from the XIAO nRF52840 pinout + the specific E22
// carrier board you're targeting. The values below are placeholders.
#define PIN_LORA_NSS            -1
#define PIN_LORA_SCK            -1
#define PIN_LORA_MOSI           -1
#define PIN_LORA_MISO           -1
#define PIN_LORA_RESET          -1
#define PIN_LORA_BUSY           -1
#define PIN_LORA_DIO1           -1
#define PIN_LORA_RXEN           -1
#define PIN_LORA_TXEN           -1

#define PIN_BATTERY             -1
#define BATTERY_ADC_RESOLUTION  12

#define PIN_LED                 -1
#define LED_ACTIVE_HIGH         1

// ---- Default config values for first boot -------------------------
#define DEFAULT_CONFIG_FREQ_HZ          915000000UL
#define DEFAULT_CONFIG_BW_HZ            125000UL
#define DEFAULT_CONFIG_SF               10
#define DEFAULT_CONFIG_CR               5
#define DEFAULT_CONFIG_TXP_DBM          14
#define DEFAULT_CONFIG_BATT_MULT        1.0f   // TODO: calibrate
#define DEFAULT_CONFIG_DISPLAY_NAME     "Unconfigured Repeater"
