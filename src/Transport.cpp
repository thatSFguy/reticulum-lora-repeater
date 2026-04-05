// src/Transport.cpp — Reticulum transport stack lifecycle and the
// LoRaInterface that bridges Reticulum's InterfaceImpl API to the
// RadioLib-backed radio driver in Radio.cpp.
//
// Phase 2 simplifications that are still in place:
//   - No packet fragmentation (payloads > 255 bytes are dropped)
//   - No CSMA / DCD / airtime tracking — synchronous TX through
//     rlr::radio::transmit()
//   - No multi-packet RX queue — tick() drains one packet per loop
//     iteration. If the rate ever exceeds one packet per loop we'll
//     add a ring buffer in Radio.cpp.
//
// The entire radio ISR path — chip IRQ mask routing, DIO1 host
// interrupt attachment, deferred dispatch — is handled inside
// RadioLib via setPacketReceivedAction(). Transport just polls
// radio::rx_pending() + radio::read_pending() from tick().

#include "Transport.h"
#include "Radio.h"

#include <Arduino.h>

// microReticulum stack — pulled in via platformio.ini lib_deps
#include <Reticulum.h>
#include <Transport.h>
#include <Identity.h>
#include <Destination.h>
#include <Interface.h>
#include <Bytes.h>
#include <Utilities/OS.h>

// microStore filesystem backend for persistent identity + path table.
// Top-level InternalFileSystem.h include is REQUIRED for PlatformIO's
// LDF to discover the Adafruit framework-bundled library — without it
// the transitive include inside microStore's adapter header isn't
// scanned aggressively enough.
#include <InternalFileSystem.h>
#include <microStore/Adapters/InternalFSFileSystem.h>

// File-scope microStore::FileSystem wrapping an InternalFS impl.
// Lives at file scope so its shared_ptr stays alive for the lifetime
// of the program; declaring it inside init() would leave a dead
// pointer after init() returns.
static microStore::FileSystem s_filesystem{microStore::Adapters::InternalFSFileSystem()};

// ---------------------------------------------------------------------
//  LoRaInterface — RNS InterfaceImpl that bridges to rlr::radio.
// ---------------------------------------------------------------------
class LoRaInterface : public RNS::InterfaceImpl {
public:
    LoRaInterface() : RNS::InterfaceImpl("LoRaInterface") {
        _IN  = true;
        _OUT = true;
        _HW_MTU = 508;
    }
    ~LoRaInterface() override { _name = "deleted"; }

protected:
    void handle_incoming(const RNS::Bytes& data) override {
        try {
            RNS::InterfaceImpl::handle_incoming(data);
        }
        catch (const std::bad_alloc&) {
            Serial.println("LoRaInterface::handle_incoming: bad_alloc");
        }
        catch (std::exception& e) {
            Serial.print("LoRaInterface::handle_incoming: ");
            Serial.println(e.what());
        }
    }

    void send_outgoing(const RNS::Bytes& data) override {
        try {
            if (data.size() > 255) {
                Serial.print("LoRaInterface::send_outgoing: DROPPED oversized packet (");
                Serial.print(data.size());
                Serial.println(" bytes > 255)");
                RNS::InterfaceImpl::handle_outgoing(data);
                return;
            }
            int n = rlr::radio::transmit(data.data(), data.size());
            if (n < 0) {
                Serial.println("LoRaInterface::send_outgoing: radio::transmit() error");
            }
            RNS::InterfaceImpl::handle_outgoing(data);
        }
        catch (const std::bad_alloc&) {
            Serial.println("LoRaInterface::send_outgoing: bad_alloc");
        }
        catch (std::exception& e) {
            Serial.print("LoRaInterface::send_outgoing: ");
            Serial.println(e.what());
        }
    }
};

// ---------------------------------------------------------------------
//  Module-level state
// ---------------------------------------------------------------------
static RNS::Reticulum  s_reticulum(RNS::Type::NONE);
static RNS::Interface  s_lora_interface(RNS::Type::NONE);
static bool            s_initialized = false;

// Exposed counters
static uint32_t s_packets_in  = 0;
static uint32_t s_packets_out = 0;

// RNS log callback — mirrors Reticulum's log stream to Serial so
// library-level problems are visible during bring-up.
static void on_rns_log(const char* msg, RNS::LogLevel level) {
    Serial.print(RNS::getTimeString());
    Serial.print(" [");
    Serial.print(RNS::getLevelName(level));
    Serial.print("] ");
    Serial.println(msg);
}

// ---------------------------------------------------------------------
//  Public API
// ---------------------------------------------------------------------
namespace rlr { namespace transport {

bool init(const Config& cfg) {
    (void)cfg;  // Phase 3+ will drive telemetry intervals etc. from cfg

    RNS::set_log_callback(&on_rns_log);

    // Mount and register the microStore filesystem BEFORE reticulum
    // starts. microReticulum's library code reaches into
    // RNS::Utilities::OS::get_filesystem() for identity, path_store,
    // and announce cache persistence, and throws std::runtime_error
    // if no backend is registered. init() is what actually runs
    // InternalFS.begin() — the adapter constructor only stores
    // configuration.
    Serial.println("Transport: initializing filesystem...");
    s_filesystem.init();
    Serial.println("Transport: registering filesystem...");
    RNS::Utilities::OS::register_filesystem(s_filesystem);

    // Create and register the LoRaInterface. RNS::Interface's
    // operator=(InterfaceImpl*) takes ownership of the raw pointer
    // into its internal shared_ptr — we must not delete it ourselves.
    s_lora_interface = new LoRaInterface();
    s_lora_interface.mode(RNS::Type::Interface::MODE_GATEWAY);
    RNS::Transport::register_interface(s_lora_interface);

    // Instantiate the Reticulum singleton in transport mode.
    s_reticulum = RNS::Reticulum();
    s_reticulum.transport_enabled(true);
    s_reticulum.probe_destination_enabled(true);
    s_reticulum.start();

    RNS::Utilities::OS::set_loop_callback([]() {});

    s_initialized = true;
    Serial.println("Transport: RNS initialized in transport mode");
    return true;
}

void tick() {
    if (!s_initialized) return;

    // Drain any pending RX packet from the radio and feed it to the
    // interface in normal task context. rlr::radio::read_pending()
    // returns the number of bytes read (0 = no packet, >0 = packet,
    // -1 = error), and internally re-enters continuous RX on the
    // chip before returning, so there's no RX downtime between
    // packet boundaries.
    uint8_t rx_buf[256];
    int rx_len = rlr::radio::read_pending(rx_buf, sizeof(rx_buf));
    if (rx_len > 0) {
        s_packets_in++;
        if (s_lora_interface) {
            RNS::Bytes data(rx_buf, (size_t)rx_len);
            s_lora_interface.handle_incoming(data);
        }
    }

    try {
        s_reticulum.loop();
    }
    catch (const std::bad_alloc&) {
        Serial.println("Transport::tick: bad_alloc in reticulum.loop()");
    }
    catch (std::exception& e) {
        Serial.print("Transport::tick: ");
        Serial.println(e.what());
    }
}

uint32_t path_count()        { return 0; } // TODO Phase 4
uint32_t destination_count() { return 0; } // TODO Phase 4
uint32_t packets_in()        { return s_packets_in; }
uint32_t packets_out()       { return s_packets_out; }

}} // namespace rlr::transport
