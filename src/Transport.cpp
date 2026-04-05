// src/Transport.cpp — Reticulum transport stack lifecycle and the
// LoRaInterface that bridges Reticulum's InterfaceImpl API to the
// SX1262 driver.
//
// Ported from microReticulum_Faketec_Repeater's RNode_Firmware.ino
// (lines 92-160 for LoRaInterface, 650-730 for the RNS init block)
// with Phase 2 simplifications:
//
//   - No packet fragmentation (payloads > 255 bytes are dropped).
//     Reticulum defaults are well under the raw LoRa payload limit
//     at SF10/250kHz so this is fine for v0.1. Fragmentation can
//     be added later as a distinct module.
//   - No CSMA / DCD / airtime tracking. The sibling project's
//     tx_queue_handler + channel stats are omitted. This firmware
//     transmits synchronously from send_outgoing().
//   - Single-packet RX latch instead of a ring buffer — on-chip
//     FIFO plus one-packet staging is enough at Reticulum's
//     typical announce + data rates.
//
// Future phases will grow these if real-world traffic needs them.

#include "Transport.h"
#include "Radio.h"
#include "drivers/sx126x.h"

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
// Matches the sibling project's pattern at RNode_Firmware.ino:319-320:
// declare the FileSystem as a file-scope global so its InternalFS
// impl initializes during static init, then register it with
// RNS::Utilities::OS before reticulum.start() so every file op
// inside the library resolves to a real backend.
//
// The top-level <InternalFileSystem.h> include is REQUIRED (not
// redundant): PlatformIO's LDF in chain/deep mode won't discover
// the Adafruit framework-bundled InternalFileSystem library from a
// transitive include inside microStore's adapter header. Keeping
// the #include here at src/ scope makes the LDF pick it up the same
// way the sibling project gets it from Utilities.h:23.
#include <InternalFileSystem.h>
#include <microStore/Adapters/InternalFSFileSystem.h>

// File-scope microStore::FileSystem wrapping an InternalFS impl.
// WARNING: keep this at file scope. If you construct it inside
// Transport::init() instead, it goes out of scope when init() returns
// and every subsequent library file op dereferences a dead shared_ptr.
static microStore::FileSystem s_filesystem{microStore::Adapters::InternalFSFileSystem()};

// ---------------------------------------------------------------------
//  RX staging — the sx126x driver's onReceive callback runs in ISR
//  context and cannot safely allocate or call into Reticulum. We latch
//  the packet bytes into a fixed buffer here and drain it from tick()
//  in normal task context.
// ---------------------------------------------------------------------
static constexpr size_t RX_STAGE_MAX = 320;    // generous upper bound for unframented LoRa packets
static uint8_t  s_rx_buf[RX_STAGE_MAX];
static volatile size_t  s_rx_len = 0;
static volatile bool    s_rx_ready = false;

// ---------------------------------------------------------------------
//  RX diagnostic counters — incremented inside the ISR, printed from
//  tick(). These answer the question "is the ISR firing at all?"
//  without needing a scope on the DIO1 pin. Each counter isolates a
//  different failure mode so we can localize bugs in the radio path.
// ---------------------------------------------------------------------
static volatile uint32_t s_isr_fire_count      = 0;  // every ISR entry
static volatile uint32_t s_isr_bad_size_count  = 0;  // packet_size <= 0 or > RX_STAGE_MAX
static volatile uint32_t s_isr_dropped_count   = 0;  // dropped because s_rx_ready was still set
static volatile uint32_t s_isr_latched_count   = 0;  // successfully latched into staging buffer

// ---------------------------------------------------------------------
//  LoRaInterface — RNS InterfaceImpl bridging to the sx126x driver.
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
            // Phase 2 simplification: synchronous single-packet TX
            // through the driver. The sx126x::beginPacket() /
            // write() / endPacket() sequence blocks until the chip
            // reports TX done, then returns the radio to RX mode.
            sx126x& radio = rlr::radio::driver();
            if (data.size() > 255) {
                Serial.print("LoRaInterface::send_outgoing: DROPPED oversized packet (");
                Serial.print(data.size());
                Serial.println(" bytes > 255)");
                // Still call handle_outgoing so Reticulum's bookkeeping
                // stays consistent — it sees the packet as "delivered to
                // the interface" even though we couldn't radio it.
                RNS::InterfaceImpl::handle_outgoing(data);
                return;
            }
            radio.beginPacket();
            radio.write(data.data(), data.size());
            radio.endPacket();
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
//
//  s_lora_interface is an RNS::Interface wrapper holding a shared_ptr
//  to a LoRaInterface impl. Assigning `s_lora_interface = new LoRaInterface()`
//  transfers ownership into the shared_ptr via Interface's operator=.
//  We never hold a raw LoRaInterface* — everything goes through the
//  wrapper's public handle_incoming / mode / register_interface API.
// ---------------------------------------------------------------------
static RNS::Reticulum  s_reticulum(RNS::Type::NONE);
static RNS::Interface  s_lora_interface(RNS::Type::NONE);
static bool            s_initialized = false;

// Counters exposed to STATUS command
static uint32_t s_packets_in  = 0;
static uint32_t s_packets_out = 0;

// ---------------------------------------------------------------------
//  sx126x ISR callback — fires when the radio has received a packet.
//  ISR-SAFE: no heap, no Serial.print, no Reticulum calls. Just copy
//  the FIFO contents into the staging buffer and set the ready flag.
// ---------------------------------------------------------------------
static void on_radio_receive(int packet_size) {
    s_isr_fire_count++;                             // every ISR entry — tells us if it fires at all
    if (packet_size <= 0 || (size_t)packet_size > RX_STAGE_MAX) {
        s_isr_bad_size_count++;
        return;
    }
    if (s_rx_ready) {
        s_isr_dropped_count++;                      // previous packet not drained yet, drop this one
        return;
    }
    sx126x& radio = rlr::radio::driver();
    for (int i = 0; i < packet_size; i++) {
        s_rx_buf[i] = (uint8_t)radio.read();
    }
    s_rx_len = (size_t)packet_size;
    s_rx_ready = true;
    s_isr_latched_count++;
}

// ---------------------------------------------------------------------
//  RNS log callback — mirrors Reticulum's log stream to Serial so
//  boot problems are visible during bring-up.
// ---------------------------------------------------------------------
static void on_rns_log(const char* msg, RNS::LogLevel level) {
    Serial.print(RNS::getTimeString());
    Serial.print(" [");
    Serial.print(RNS::getLevelName(level));
    Serial.print("] ");
    Serial.println(msg);
}

// ---------------------------------------------------------------------
//  Transport::init
// ---------------------------------------------------------------------
namespace rlr { namespace transport {

bool init(const Config& cfg) {
    (void)cfg;  // Phase 3+ will drive telemetry intervals etc. from cfg

    RNS::set_log_callback(&on_rns_log);

    // Hand the microReticulum library a filesystem backend BEFORE
    // reticulum.start() runs — identity persistence, path_store init,
    // and announce cache all resolve through OS::get_filesystem() and
    // throw std::runtime_error if no backend is registered.
    //
    // IMPORTANT: s_filesystem.init() must be called explicitly. The
    // InternalFSFileSystem adapter's constructor does NOT mount the
    // internal flash partition — it only stores pins. init() is what
    // actually runs InternalFS.begin(). Without this call every file
    // op into the (unmounted) littlefs fails and the firmware boots
    // in complete silence. Every microReticulum example does this
    // same two-step init + register dance.
    Serial.println("Transport: initializing filesystem...");
    s_filesystem.init();
    Serial.println("Transport: registering filesystem...");
    RNS::Utilities::OS::register_filesystem(s_filesystem);

    // Register the RX callback BEFORE the radio enters continuous RX
    // mode. sx126x::onReceive() does two load-bearing things:
    //   1. Sends OP_SET_IRQ_FLAGS_6X to configure which chip-internal
    //      IRQ events (RX_DONE, etc.) route to which DIO pin. Without
    //      this the DIO1 line never goes high on packet reception.
    //   2. attachInterrupt()s the host-side handler on DIO1.
    // Both must happen before radio::start_rx() is called from
    // main.cpp::setup(). If the chip enters RX first and we register
    // the callback after, packets arrive but nothing sees them.
    Serial.println("Transport: registering RX callback...");
    rlr::radio::driver().onReceive(&on_radio_receive);

    // Create and register the LoRaInterface. RNS::Interface's
    // operator=(InterfaceImpl*) transfers ownership of the raw pointer
    // into its internal shared_ptr — we must not delete it ourselves.
    s_lora_interface = new LoRaInterface();
    s_lora_interface.mode(RNS::Type::Interface::MODE_GATEWAY);
    RNS::Transport::register_interface(s_lora_interface);

    // Instantiate the Reticulum singleton in transport mode.
    s_reticulum = RNS::Reticulum();
    s_reticulum.transport_enabled(true);           // always on — this firmware is transport-only
    s_reticulum.probe_destination_enabled(true);
    s_reticulum.start();

    // Hand microReticulum's OS shim a loop callback so anything it
    // needs to do between our tick() calls gets scheduled.
    RNS::Utilities::OS::set_loop_callback([]() {});

    s_initialized = true;
    Serial.println("Transport: RNS initialized in transport mode");
    return true;
}

void tick() {
    if (!s_initialized) return;

    // Drain the RX staging buffer. We copy out under a brief IRQ-off
    // window so the ISR can't scribble while we're reading, then
    // hand the bytes to Reticulum in normal task context.
    if (s_rx_ready) {
        uint8_t local_buf[RX_STAGE_MAX];
        size_t  local_len;
        noInterrupts();
        local_len = s_rx_len;
        memcpy(local_buf, s_rx_buf, local_len);
        s_rx_ready = false;
        interrupts();

        s_packets_in++;
        if (s_lora_interface) {
            RNS::Bytes data(local_buf, local_len);
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

uint32_t path_count()        { return 0; } // TODO Phase 4: plumb from RNS::Transport path table size
uint32_t destination_count() { return 0; } // TODO Phase 4
uint32_t packets_in()        { return s_packets_in; }
uint32_t packets_out()       { return s_packets_out; }

uint32_t rx_isr_fires()      { return s_isr_fire_count; }
uint32_t rx_isr_bad_size()   { return s_isr_bad_size_count; }
uint32_t rx_isr_dropped()    { return s_isr_dropped_count; }
uint32_t rx_isr_latched()    { return s_isr_latched_count; }

}} // namespace rlr::transport
