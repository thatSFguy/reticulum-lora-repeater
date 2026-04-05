// src/Radio.cpp — stub. Phase 2 copies the working sx126x init path
// from microReticulum_Faketec_Repeater/RNode_Firmware.ino + sx126x.cpp
// and stitches it to the board header macros.
#include "Radio.h"

namespace rlr { namespace radio {

static bool s_online = false;

bool init_hardware() {
    // TODO Phase 2:
    //   - pinMode(PIN_VEXT_EN, OUTPUT); digitalWrite(PIN_VEXT_EN, HIGH); delay(VEXT_SETTLE_MS);
    //   - SPI.setPins(PIN_LORA_MISO, PIN_LORA_SCK, PIN_LORA_MOSI); SPI.begin();
    //   - sx126x.setPins(PIN_LORA_NSS, PIN_LORA_RESET, PIN_LORA_DIO1, PIN_LORA_BUSY, PIN_LORA_RXEN);
    //   - sx126x.preInit()  — sync-word probe
    //   - Return preInit success
    return false;
}

bool begin(const Config& cfg) {
    // TODO Phase 2:
    //   - sx126x.begin(cfg.freq_hz)
    //   - setBandwidth / setSpreadingFactor / setCodingRate4 / setTxPower from cfg
    //   - sx126x.setSyncWord(0x1424)
    //   - sx126x.enableCrc()
    //   - TCXO voltage from RADIO_TCXO_VOLTAGE_MV in board header
    //   - DIO2_AS_RF_SWITCH from RADIO_DIO2_AS_RF_SWITCH
    //   - receive() continuous
    (void)cfg;
    s_online = false;
    return s_online;
}

bool online() { return s_online; }

void stop() { s_online = false; }

}} // namespace rlr::radio
