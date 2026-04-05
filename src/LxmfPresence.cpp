// src/LxmfPresence.cpp — stub. Phase 5 copies the working msgpack
// encoder + announce_lxmf_presence() from the sibling project and
// reads display_name from Config instead of BAKED_LXMF_DISPLAY_NAME.
#include "LxmfPresence.h"

namespace rlr { namespace lxmf_presence {

bool init(const Config& cfg) {
    // TODO Phase 5:
    //   - lxmf_presence_destination = RNS::Destination(identity, IN, SINGLE, "lxmf", "delivery")
    (void)cfg;
    return false;
}

void tick(const Config& cfg) {
    // TODO Phase 5: millis()-based interval check
    (void)cfg;
}

void announce_now(const Config& cfg) {
    // TODO Phase 5: hand-roll msgpack [bin8(name), nil] and call announce
    //   buf[0] = 0x92                  // fixarray, 2 elements
    //   buf[1] = 0xc4                  // bin8
    //   buf[2] = name_len
    //   memcpy(buf+3, cfg.display_name, name_len)
    //   buf[3+name_len] = 0xc0         // nil (stamp_cost)
    //   lxmf_presence_destination.announce(RNS::Bytes(buf, 4 + name_len))
    (void)cfg;
}

}} // namespace rlr::lxmf_presence
