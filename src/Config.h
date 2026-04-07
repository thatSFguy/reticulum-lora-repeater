#pragma once
// =====================================================================
//  src/Config.h — runtime configuration persisted to internal flash
//
//  The Config struct is the ONLY place firmware settings live. Every
//  knob that used to be a BAKED_* build flag in the sibling project
//  is a field here. Loaded from /config_store_0.dat at boot, edited
//  over the serial console, committed back to flash, reboot applies.
//
//  Layout is packed binary. Schema versioning in the `version` field
//  lets future additions append fields without invalidating old
//  saved configs.
// =====================================================================

#include <stdint.h>
#include <Print.h>

namespace rlr {

#pragma pack(push, 1)
struct Config {
    // ---- schema ----
    uint16_t version;               // 1 = original, 2 = +BT/location
    uint16_t _reserved;

    // ---- radio ----
    uint32_t freq_hz;               // e.g. 904375000
    uint32_t bw_hz;                 // e.g. 250000
    uint8_t  sf;                    // 7..12
    uint8_t  cr;                    // 5..8 (denominator, 4/5..4/8)
    int8_t   txp_dbm;               // -9..+22 at the SX1262 core pin
    uint8_t  flags;                 // bit 0 = telemetry, bit 1 = lxmf, bit 2 = heartbeat

    // ---- battery ----
    float    batt_mult;             // ADC-raw → mV scaling factor

    // ---- periodic tasks ----
    uint32_t tele_interval_ms;      // battery telemetry cadence
    uint32_t lxmf_interval_ms;      // LXMF presence cadence

    // ---- bluetooth ----
    uint32_t bt_pin;                // BLE pairing PIN 0-999999, 0 = no PIN

    // ---- location ----
    int32_t  latitude_udeg;         // microdegrees (deg * 1e6), 0 = not set
    int32_t  longitude_udeg;        // microdegrees (deg * 1e6), 0 = not set
    int32_t  altitude_m;            // meters above mean sea level, 0 = not set

    // ---- identity ----
    char     display_name[32];      // NUL-terminated UTF-8

    // ---- integrity ----
    uint32_t crc32;                 // CRC over all preceding bytes
};
#pragma pack(pop)

static_assert(sizeof(Config) <= 128, "Config grew unexpectedly — review schema version bump");

// Flag bits
enum : uint8_t {
    CONFIG_FLAG_TELEMETRY  = 1 << 0,
    CONFIG_FLAG_LXMF       = 1 << 1,
    CONFIG_FLAG_HEARTBEAT  = 1 << 2,
    CONFIG_FLAG_BT_ENABLED = 1 << 3,
};

namespace config {

// Populate `out` with the board's hardcoded defaults from the
// pre-included board header. Does NOT touch flash. Used on first boot
// and after CONFIG RESET before COMMIT.
void defaults(Config& out);

// Load config from microStore. Returns true on success. On failure
// (missing, wrong version, bad CRC), `out` is populated with defaults
// and the function returns false — callers should treat this as
// "first boot" and carry on.
bool load(Config& out);

// Persist config to microStore. Computes CRC before writing.
// Returns true on success.
bool save(const Config& in);

// Convenience: load-or-defaults in one call. Always populates `out`.
void load_or_defaults(Config& out);

// Validate a Config's CRC + basic field ranges. Used by SerialConsole
// before accepting a COMMIT. Returns true if the config is safe to
// persist and boot from.
bool validate(const Config& cfg);

// Set/get helpers for the serial console. They operate on a staging
// copy; nothing is persisted until save() is called.
bool set_field(Config& cfg, const char* key, const char* value);
void print_fields(const Config& cfg, Print& out);  // writes "key=value" lines to out

} // namespace config
} // namespace rlr
