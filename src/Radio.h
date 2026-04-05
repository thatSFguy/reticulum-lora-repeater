#pragma once
// src/Radio.h — SX1262 radio lifecycle and packet I/O, implemented on
// top of RadioLib's SX1262 class. Board-specific pin numbers, TCXO
// voltage, RF-switch wiring, and VEXT rail come from the pre-included
// board header (include/board/<name>.h); this file is board-agnostic.
//
// This header replaces an earlier sibling-project sx126x driver port
// that turned out to have too many per-board quirks and deferred-
// dispatch gotchas. RadioLib is what Meshtastic/MeshCore use and
// handles all the low-level chip init, DIO1 interrupt routing, and
// callback plumbing transparently. See docs/TROUBLESHOOTING.md
// items #12-#14 for the failure modes that pushed us to switch.

#include "Config.h"
#include <stdint.h>
#include <stddef.h>

namespace rlr { namespace radio {

// Assert VEXT_EN (if the board has it), configure SPI pins, construct
// the RadioLib Module + SX1262 objects. Does NOT talk to the chip
// yet — that's begin()'s job. Returns true on success.
bool init_hardware();

// Apply the runtime Config's freq/BW/SF/CR/TXP, TCXO voltage (from
// RADIO_TCXO_VOLTAGE_MV in the board header), sync word, and DIO2-
// as-RF-switch setting, and leave the radio in STANDBY. Returns
// true on success.
bool begin(const Config& cfg);

// Wire the RX interrupt handler and enter continuous RX mode.
// Call after begin(). Returns true on success.
bool start_rx();

// Query whether the radio passed begin() (i.e. is configured).
bool online();

// Put the radio into sleep/standby. Reversible via begin().
void stop();

// Called from loop(): returns true if the ISR has latched a packet
// that is ready to be drained via read_pending().
bool rx_pending();

// Drain a pending RX packet into `buf`. Returns the number of bytes
// read on success, 0 if no packet is ready, -1 on error. Always
// re-enters continuous RX on the chip before returning so subsequent
// packets are captured. `bufsize` should be at least 256 for safety.
int read_pending(uint8_t* buf, size_t bufsize);

// Synchronously transmit a packet. Blocks until the chip reports
// TX done, then re-enters continuous RX. Returns the number of bytes
// transmitted on success, -1 on error.
int transmit(const uint8_t* buf, size_t len);

}} // namespace rlr::radio
