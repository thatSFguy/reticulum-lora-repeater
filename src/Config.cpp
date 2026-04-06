// src/Config.cpp — runtime configuration load/save via microStore.
//
// The Config struct is serialized as a single packed binary record to
// /config.bin in the internal littlefs partition. No key-value store,
// no framing — just the struct with a CRC32 in its trailing field.
// Atomic enough for our use case because writes are single microStore
// operations; if power is lost mid-write the CRC check on next boot
// will reject the partial record and we fall back to defaults.
//
// Phase 3 implements load/save. Phase 4 (SerialConsole) will add the
// set_field/print_fields helpers that the CONFIG GET/SET commands call.

#include "Config.h"
#include "Arduino.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include <Bytes.h>
#include <Utilities/OS.h>

namespace rlr { namespace config {

// The on-disk path for the persisted Config record. Kept at the root
// of the littlefs partition alongside the Reticulum path_store and
// transport_identity files that microReticulum manages on its own.
static constexpr const char* CONFIG_PATH = "/config.bin";

// --- CRC-32 helper (zlib polynomial 0xEDB88320, reflected) -------
//
// Tiny local implementation so we don't pull in a dependency just for
// 40 bytes of table-free CRC. Used exclusively to stamp + verify the
// crc32 field inside the Config struct; if the struct grows or its
// layout changes in a future schema version, the CRC covers all
// preceding bytes and an old firmware reading a new file (or vice
// versa) will reject on CRC mismatch and fall back to defaults.
static uint32_t crc32_of(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            // Branchless: propagate LSB to mask
            uint32_t mask = -(int32_t)(crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

// --- defaults ----------------------------------------------------

void defaults(Config& out) {
    memset(&out, 0, sizeof(out));
    out.version          = 1;
    out.freq_hz          = DEFAULT_CONFIG_FREQ_HZ;
    out.bw_hz            = DEFAULT_CONFIG_BW_HZ;
    out.sf               = DEFAULT_CONFIG_SF;
    out.cr               = DEFAULT_CONFIG_CR;
    out.txp_dbm          = DEFAULT_CONFIG_TXP_DBM;
    out.flags            = CONFIG_FLAG_TELEMETRY | CONFIG_FLAG_LXMF | CONFIG_FLAG_HEARTBEAT;
    out.batt_mult        = DEFAULT_CONFIG_BATT_MULT;
    out.tele_interval_ms = 10800000UL;   // 3 h
    out.lxmf_interval_ms = 1800000UL;    // 30 min
    // Build a unique default name from "Rptr-" + last 12 hex chars of
    // the nRF52840's factory-programmed device ID (FICR). This gives
    // every node a distinct name out of the box, making it clear in
    // Sideband/MeshChat that it's a repeater (not a chat endpoint)
    // and which physical device it is. Users can override via CONFIG SET.
    {
        uint32_t id0 = NRF_FICR->DEVICEID[0];
        uint32_t id1 = NRF_FICR->DEVICEID[1];
        // Take lower 24 bits of each word → 12 hex chars
        snprintf(out.display_name, sizeof(out.display_name),
                 "Rptr-%04X%08X", (unsigned)(id1 & 0xFFFF), (unsigned)id0);
    }
    out.crc32            = 0;            // populated on save
}

// --- validate ----------------------------------------------------

bool validate(const Config& cfg) {
    if (cfg.version != 1)                               return false;
    if (cfg.sf < 7   || cfg.sf > 12)                    return false;
    if (cfg.cr < 5   || cfg.cr > 8)                     return false;
    if (cfg.txp_dbm < -9 || cfg.txp_dbm > 22)           return false;
    if (cfg.freq_hz < 100000000UL || cfg.freq_hz > 1100000000UL) return false;
    if (cfg.bw_hz   < 7800UL      || cfg.bw_hz   > 500000UL)     return false;
    // display_name must be NUL-terminated within the buffer
    bool terminated = false;
    for (size_t i = 0; i < sizeof(cfg.display_name); i++) {
        if (cfg.display_name[i] == '\0') { terminated = true; break; }
    }
    if (!terminated)                                    return false;
    return true;
}

// --- load --------------------------------------------------------
//
// Reads /config.bin, validates size, CRC, and field ranges. Returns
// true on success; on any failure `out` is left untouched (caller is
// expected to call defaults() or treat as first boot).
bool load(Config& out) {
    try {
        if (!RNS::Utilities::OS::file_exists(CONFIG_PATH)) {
            Serial.println("Config: /config.bin not present — first boot");
            return false;
        }

        RNS::Bytes data;
        size_t n = RNS::Utilities::OS::read_file(CONFIG_PATH, data);
        if (n != sizeof(Config)) {
            Serial.print("Config: /config.bin size mismatch (");
            Serial.print(n);
            Serial.print(" bytes, expected ");
            Serial.print(sizeof(Config));
            Serial.println(")");
            return false;
        }

        Config tmp;
        memcpy(&tmp, data.data(), sizeof(Config));

        // CRC32 covers every byte before the crc32 field itself.
        const size_t crc_covered = sizeof(Config) - sizeof(uint32_t);
        uint32_t want   = tmp.crc32;
        uint8_t zeroed_tail[sizeof(Config)];
        memcpy(zeroed_tail, &tmp, sizeof(Config));
        memset(zeroed_tail + crc_covered, 0, sizeof(uint32_t));  // zero out the crc field before hashing
        uint32_t actual = crc32_of(zeroed_tail, crc_covered);
        if (actual != want) {
            Serial.print("Config: CRC mismatch (got 0x");
            Serial.print(actual, HEX);
            Serial.print(", want 0x");
            Serial.print(want, HEX);
            Serial.println(")");
            return false;
        }

        if (!validate(tmp)) {
            Serial.println("Config: CRC OK but field validation failed");
            return false;
        }

        out = tmp;
        Serial.println("Config: loaded from /config.bin");
        return true;
    }
    catch (const std::exception& e) {
        Serial.print("Config::load exception: ");
        Serial.println(e.what());
        return false;
    }
}

// --- save --------------------------------------------------------
//
// Stamps a fresh CRC over the struct and writes /config.bin. Returns
// true on success. Validation is done up front so a caller cannot
// persist garbage into flash.
bool save(const Config& in) {
    try {
        if (!validate(in)) {
            Serial.println("Config::save: validation failed, refusing to persist");
            return false;
        }

        Config tmp = in;
        tmp.version = 1;
        tmp.crc32   = 0;
        const size_t crc_covered = sizeof(Config) - sizeof(uint32_t);
        tmp.crc32 = crc32_of(reinterpret_cast<const uint8_t*>(&tmp), crc_covered);

        RNS::Bytes data(reinterpret_cast<const uint8_t*>(&tmp), sizeof(Config));
        size_t written = RNS::Utilities::OS::write_file(CONFIG_PATH, data);
        if (written != sizeof(Config)) {
            Serial.print("Config::save: short write (");
            Serial.print(written);
            Serial.print("/");
            Serial.print(sizeof(Config));
            Serial.println(" bytes)");
            return false;
        }
        Serial.print("Config: saved to /config.bin (");
        Serial.print(written);
        Serial.println(" bytes)");
        return true;
    }
    catch (const std::exception& e) {
        Serial.print("Config::save exception: ");
        Serial.println(e.what());
        return false;
    }
}

// --- load_or_defaults -------------------------------------------
//
// Three-state logic:
//   1. File exists + CRC valid + fields valid → load() wins, keep it.
//   2. File missing (first boot) → populate defaults, persist them so
//      the next boot is a normal load path.
//   3. File corrupt (wrong size, bad CRC, bad fields) → load() rejects,
//      populate defaults, persist. The corrupt file is silently
//      overwritten — if that ever becomes a problem in production
//      we can add a "/config.bin.bak" rescue slot.
void load_or_defaults(Config& out) {
    if (load(out)) return;
    Serial.println("Config: falling back to board defaults");
    defaults(out);
    // Persist the freshly-defaulted config so the next boot takes
    // the fast path. If save() fails we still return the in-memory
    // defaults and carry on.
    if (!save(out)) {
        Serial.println("Config: warning — defaults could not be persisted");
    }
}

// --- set_field / print_fields ------------------------------------
//
// Drives the CONFIG GET / CONFIG SET <key> <val> commands in
// SerialConsole. Keys are flat dotless names matching the Config
// struct field names. Boolean flag bits are exposed as individual
// keys (telemetry / lxmf / heartbeat) that set/clear bits inside
// `flags`. On any parse/range failure set_field() returns false
// without mutating cfg — the caller reports ERR to the user and the
// staging record stays coherent.

static bool streq(const char* a, const char* b) {
    return strcmp(a, b) == 0;
}

// Parse a boolean token: 0/1, true/false, on/off, yes/no.
static bool parse_bool(const char* v, bool& out) {
    if (streq(v, "1") || streq(v, "true") || streq(v, "on")  || streq(v, "yes")) { out = true;  return true; }
    if (streq(v, "0") || streq(v, "false")|| streq(v, "off") || streq(v, "no") ) { out = false; return true; }
    return false;
}

bool set_field(Config& cfg, const char* key, const char* value) {
    if (!key || !value) return false;

    if (streq(key, "display_name")) {
        size_t n = strlen(value);
        if (n == 0 || n >= sizeof(cfg.display_name)) return false;
        memset(cfg.display_name, 0, sizeof(cfg.display_name));
        memcpy(cfg.display_name, value, n);
        return true;
    }
    if (streq(key, "freq_hz")) {
        char* end = nullptr;
        unsigned long v = strtoul(value, &end, 10);
        if (end == value || *end != '\0') return false;
        if (v < 100000000UL || v > 1100000000UL) return false;
        cfg.freq_hz = (uint32_t)v;
        return true;
    }
    if (streq(key, "bw_hz")) {
        char* end = nullptr;
        unsigned long v = strtoul(value, &end, 10);
        if (end == value || *end != '\0') return false;
        if (v < 7800UL || v > 500000UL) return false;
        cfg.bw_hz = (uint32_t)v;
        return true;
    }
    if (streq(key, "sf")) {
        char* end = nullptr;
        long v = strtol(value, &end, 10);
        if (end == value || *end != '\0') return false;
        if (v < 7 || v > 12) return false;
        cfg.sf = (uint8_t)v;
        return true;
    }
    if (streq(key, "cr")) {
        char* end = nullptr;
        long v = strtol(value, &end, 10);
        if (end == value || *end != '\0') return false;
        if (v < 5 || v > 8) return false;
        cfg.cr = (uint8_t)v;
        return true;
    }
    if (streq(key, "txp_dbm")) {
        char* end = nullptr;
        long v = strtol(value, &end, 10);
        if (end == value || *end != '\0') return false;
        if (v < -9 || v > 22) return false;
        cfg.txp_dbm = (int8_t)v;
        return true;
    }
    if (streq(key, "batt_mult")) {
        char* end = nullptr;
        float v = strtof(value, &end);
        if (end == value || *end != '\0') return false;
        if (v <= 0.0f || v > 10.0f) return false;
        cfg.batt_mult = v;
        return true;
    }
    if (streq(key, "tele_interval_ms")) {
        char* end = nullptr;
        unsigned long v = strtoul(value, &end, 10);
        if (end == value || *end != '\0') return false;
        cfg.tele_interval_ms = (uint32_t)v;
        return true;
    }
    if (streq(key, "lxmf_interval_ms")) {
        char* end = nullptr;
        unsigned long v = strtoul(value, &end, 10);
        if (end == value || *end != '\0') return false;
        cfg.lxmf_interval_ms = (uint32_t)v;
        return true;
    }
    if (streq(key, "telemetry") || streq(key, "lxmf") || streq(key, "heartbeat")) {
        bool b;
        if (!parse_bool(value, b)) return false;
        uint8_t bit = streq(key, "telemetry") ? CONFIG_FLAG_TELEMETRY
                    : streq(key, "lxmf")      ? CONFIG_FLAG_LXMF
                                              : CONFIG_FLAG_HEARTBEAT;
        if (b) cfg.flags |=  bit;
        else   cfg.flags &= ~bit;
        return true;
    }
    return false;
}

void print_fields(const Config& cfg) {
    Serial.print("display_name=");     Serial.println(cfg.display_name);
    Serial.print("freq_hz=");          Serial.println(cfg.freq_hz);
    Serial.print("bw_hz=");            Serial.println(cfg.bw_hz);
    Serial.print("sf=");               Serial.println(cfg.sf);
    Serial.print("cr=");               Serial.println(cfg.cr);
    Serial.print("txp_dbm=");          Serial.println(cfg.txp_dbm);
    Serial.print("batt_mult=");        Serial.println(cfg.batt_mult, 4);
    Serial.print("tele_interval_ms="); Serial.println(cfg.tele_interval_ms);
    Serial.print("lxmf_interval_ms="); Serial.println(cfg.lxmf_interval_ms);
    Serial.print("telemetry=");        Serial.println((cfg.flags & CONFIG_FLAG_TELEMETRY) ? 1 : 0);
    Serial.print("lxmf=");             Serial.println((cfg.flags & CONFIG_FLAG_LXMF)      ? 1 : 0);
    Serial.print("heartbeat=");        Serial.println((cfg.flags & CONFIG_FLAG_HEARTBEAT) ? 1 : 0);
}

}} // namespace rlr::config
