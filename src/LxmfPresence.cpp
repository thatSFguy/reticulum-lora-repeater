// src/LxmfPresence.cpp — periodic LXMF presence announces so
// MeshChat / Sideband / NomadNet show this node by name in their
// network visualizers. Ported from the sibling project's
// announce_lxmf_presence(); display name now comes from the runtime
// Config instead of BAKED_LXMF_DISPLAY_NAME.
//
// Wire format is confirmed against the reference implementation in
// micropython-reticulum/firmware/urns/lxmf.py:
//
//   APP_NAME = "lxmf"
//   Destination(identity, IN, SINGLE, APP_NAME, "delivery")
//   app_data = umsgpack.packb([display_name.encode("utf-8"), stamp_cost])
//
// msgpack layout for a name up to 255 bytes:
//
//   0x92                 fixarray, 2 elements
//   0xc4 <len> <bytes>   bin8, display_name as UTF-8
//   0xc0                 nil (stamp_cost)

#include "LxmfPresence.h"
#include "Radio.h"

#include <Arduino.h>
#include <string.h>

#include <Reticulum.h>
#include <Transport.h>
#include <Destination.h>
#include <Bytes.h>

namespace rlr { namespace lxmf_presence {

static RNS::Destination s_dest(RNS::Type::NONE);
static bool             s_ready    = false;
static uint32_t         s_last_ms  = 0;
static constexpr uint32_t FIRST_MS = 15000UL;   // 15 s after init

bool init(const Config& cfg) {
    (void)cfg;
    try {
        s_dest = RNS::Destination(
            RNS::Transport::identity(),
            RNS::Type::Destination::IN,
            RNS::Type::Destination::SINGLE,
            "lxmf", "delivery");
        s_ready = true;
        Serial.print("LxmfPresence: destination hash ");
        Serial.println(s_dest.hash().toHex().c_str());
        return true;
    }
    catch (const std::exception& e) {
        Serial.print("LxmfPresence::init exception: ");
        Serial.println(e.what());
        s_ready = false;
        return false;
    }
}

void announce_now(const Config& cfg) {
    if (!s_ready) return;

    const char* name = cfg.display_name;
    size_t name_len  = strlen(name);
    // Cap to 200 so the whole msgpack blob fits in <=205 bytes and
    // leaves headroom inside the announce's app_data framing.
    if (name_len > 200) name_len = 200;

    uint8_t buf[224];
    size_t i = 0;
    buf[i++] = 0x92;                   // fixarray, 2 elements
    buf[i++] = 0xc4;                   // bin8
    buf[i++] = (uint8_t)name_len;
    memcpy(buf + i, name, name_len);
    i += name_len;
    buf[i++] = 0xc0;                   // nil (stamp_cost)

    Serial.print("LxmfPresence: announcing as \"");
    Serial.print(name);
    Serial.println("\"");
    try {
        s_dest.announce(RNS::Bytes(buf, i));
    }
    catch (const std::exception& e) {
        Serial.print("LxmfPresence::announce exception: ");
        Serial.println(e.what());
    }
}

void tick(const Config& cfg) {
    if (!s_ready) return;
    if ((cfg.flags & CONFIG_FLAG_LXMF) == 0) return;
    if (!rlr::radio::online()) return;

    uint32_t now = millis();
    bool due;
    if (s_last_ms == 0) {
        due = (now >= FIRST_MS);
    } else {
        due = ((now - s_last_ms) >= cfg.lxmf_interval_ms);
    }
    if (!due) return;

    s_last_ms = now;
    announce_now(cfg);
}

}} // namespace rlr::lxmf_presence
