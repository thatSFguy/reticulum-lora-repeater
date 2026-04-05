#pragma once
// src/LxmfPresence.h — periodic LXMF `lxmf.delivery` announce so
// MeshChat / Sideband / NomadNet show this node with its Config
// display_name in their network visualizers.
//
// Wire format: umsgpack.packb([display_name_bytes, stamp_cost_or_nil])
// confirmed against micropython-reticulum/firmware/urns/lxmf.py.

#include "Config.h"

namespace rlr { namespace lxmf_presence {

// Create the lxmf.delivery destination under the Transport identity.
// Must be called AFTER transport::init().
bool init(const Config& cfg);

// Called from loop(). Honors cfg.flags & CONFIG_FLAG_LXMF.
void tick(const Config& cfg);

// Force one announce immediately, for test/validation.
void announce_now(const Config& cfg);

}} // namespace rlr::lxmf_presence
