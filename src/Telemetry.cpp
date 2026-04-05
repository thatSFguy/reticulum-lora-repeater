// src/Telemetry.cpp — stub. Phase 5 copies announce_telemetry() from
// the sibling project and parameterizes cadence/calibration via Config.
#include "Telemetry.h"

namespace rlr { namespace telemetry {

bool init(const Config& cfg) {
    // TODO Phase 5:
    //   - telemetry_destination = RNS::Destination(identity, IN, SINGLE, "rlr", "telemetry")
    //   - Record the hash for the STATUS command
    (void)cfg;
    return false;
}

void tick(const Config& cfg) {
    // TODO Phase 5: millis()-based interval check with first/interval semantics
    (void)cfg;
}

void announce_now(const Config& cfg) {
    // TODO Phase 5: read_battery_mv(cfg.batt_mult), build "bat=...;up=...;..." payload,
    // call telemetry_destination.announce(RNS::bytesFromString(payload))
    (void)cfg;
}

}} // namespace rlr::telemetry
