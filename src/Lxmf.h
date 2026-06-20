#pragma once
// =====================================================================
//  src/Lxmf.h — outbound LXMF message support (opportunistic delivery).
//
//  This is the spec-compliant message-TX path the firmware previously
//  lacked. The presence announce in LxmfPresence.cpp only ever called
//  Destination::announce(); telemetry that shows up in Sideband requires
//  a real LXMF *message* carrying FIELD_TELEMETRY, addressed and
//  encrypted to a collector.
//
//  Only opportunistic single-packet delivery is implemented
//  (reticulum-specifications SPEC.md §5.1) — no Link/direct delivery,
//  no propagation nodes, no stamps. That bounds the msgpack payload to
//  ~295 bytes (SPEC, LXMessage.ENCRYPTED_PACKET_MAX_CONTENT), which is
//  plenty for a telemetry snapshot but means large messages are simply
//  rejected rather than upgraded to a Link.
// =====================================================================

#include <stdint.h>
#include <stddef.h>

namespace rlr { namespace lxmf {

// Upstream LXMF cap on the opportunistic msgpack payload (SPEC §5.1).
static constexpr size_t MAX_OPPORTUNISTIC_PAYLOAD = 295;

// Build, sign, and transmit an opportunistic LXMF message to the
// collector identified by its 16-byte lxmf.delivery destination hash.
//
//   collector_hash : 16-byte recipient destination hash. The recipient's
//                    public identity MUST already be known to Reticulum
//                    (i.e. we've heard its announce) so the body can be
//                    encrypted to it; if not, this returns false.
//   content        : optional UTF-8 message body (may be nullptr/empty).
//   fields_msgpack : pre-encoded msgpack map for the LXMF `fields`
//                    element (4th payload element). Pass nullptr/0 for an
//                    empty map {}.
//
// Returns true if a packet was handed to the radio. False on: collector
// identity unknown, oversize payload, or a Reticulum/transmit error.
// Diagnostic detail is printed to Serial.
bool send_opportunistic(const uint8_t* collector_hash,
                        const char* content,
                        const uint8_t* fields_msgpack, size_t fields_len);

} } // namespace rlr::lxmf
