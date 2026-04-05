// src/Transport.cpp — stub. Phase 2 copies the working RNS init
// block from microReticulum_Faketec_Repeater/RNode_Firmware.ino
// (lines ~650–730) and generalizes it to read from Config.
#include "Transport.h"

namespace rlr { namespace transport {

bool init(const Config& cfg) {
    // TODO Phase 2:
    //   - Initialize filesystem (microStore InternalFS)
    //   - RNS::set_log_callback(&on_log)
    //   - RNS::Transport::set_receive_packet_callback(&on_rx)
    //   - RNS::Transport::set_transmit_packet_callback(&on_tx)
    //   - lora_interface = new LoRaInterface() (copied verbatim from sibling)
    //   - RNS::Transport::register_interface(lora_interface)
    //   - reticulum = RNS::Reticulum()
    //   - reticulum.transport_enabled(true)
    //   - reticulum.probe_destination_enabled(true)
    //   - reticulum.start()
    //   - Register telemetry + lxmf presence destinations (via submodules)
    (void)cfg;
    return false;
}

void tick() {
    // TODO Phase 2: reticulum.loop()
}

uint32_t path_count()        { return 0; }
uint32_t destination_count() { return 0; }
uint32_t packets_in()        { return 0; }
uint32_t packets_out()       { return 0; }

}} // namespace rlr::transport
