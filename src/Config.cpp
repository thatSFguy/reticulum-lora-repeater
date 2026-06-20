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

#include <microReticulum/Bytes.h>
#include <microReticulum/Utilities/OS.h>

namespace rlr { namespace config {

// The on-disk path for the persisted Config record. Kept at the root
// of the littlefs partition alongside the Reticulum path_store and
// transport_identity files that microReticulum manages on its own.
static constexpr const char* CONFIG_PATH = "/config.bin";

// Size of the v1 Config struct on disk (before bt_pin/lat/lon/alt fields).
// v1 layout: version(2) + _reserved(2) + freq_hz(4) + bw_hz(4) + sf(1) +
//   cr(1) + txp_dbm(1) + flags(1) + batt_mult(4) + tele_interval_ms(4) +
//   lxmf_interval_ms(4) + display_name(32) + crc32(4) = 64 bytes.
static constexpr size_t CONFIG_V1_SIZE = 64;

// Size of the v2 Config struct on disk (before the v3 collector_hash[16]
// field). v2 and v3 share an identical byte layout from `version` through
// `display_name` (the first 76 bytes); v2 then places crc32 at offset 76,
// while v3 inserts collector_hash[16] at 76 and moves crc32 to 92.
//   v2 = 76 (fields) + 4 (crc32) = 80 bytes
//   v3 = 76 (fields) + 16 (collector_hash) + 4 (crc32) = 96 bytes
static constexpr size_t CONFIG_V2_SIZE = 80;
// Byte offset where the shared field prefix ends (start of collector_hash
// in v3 / crc32 in v2). Used by the v2->v3 migration below.
static constexpr size_t CONFIG_SHARED_PREFIX = 76;

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
    out.version          = 3;
    out.log_level        = 1;  // normal
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
    if (cfg.version != 1 && cfg.version != 2 && cfg.version != 3) return false;
    if (cfg.sf < 7   || cfg.sf > 12)                    return false;
    if (cfg.cr < 5   || cfg.cr > 8)                     return false;
    if (cfg.txp_dbm < -9 || cfg.txp_dbm > 22)           return false;
    if (cfg.freq_hz < 100000000UL || cfg.freq_hz > 1100000000UL) return false;
    if (cfg.bw_hz   < 7800UL      || cfg.bw_hz   > 500000UL)     return false;
    // v2 fields — only validate when present
    if (cfg.version >= 2) {
        if (cfg.bt_pin > 999999)                        return false;
        if (cfg.latitude_udeg  < -90000000  || cfg.latitude_udeg  > 90000000)  return false;
        if (cfg.longitude_udeg < -180000000 || cfg.longitude_udeg > 180000000) return false;
        if (cfg.altitude_m < -100000 || cfg.altitude_m > 100000)               return false;
    }
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

        // Determine the on-disk schema by file size.
        //   v1 = CONFIG_V1_SIZE (64), v2 = CONFIG_V2_SIZE (80),
        //   v3 = sizeof(Config). Older records are migrated forward.
        bool is_v1 = (n == CONFIG_V1_SIZE);
        bool is_v2 = (n == CONFIG_V2_SIZE);
        if (n != sizeof(Config) && !is_v1 && !is_v2) {
            Serial.print("Config: /config.bin size mismatch (");
            Serial.print(n);
            Serial.print(" bytes, expected ");
            Serial.print(sizeof(Config));
            Serial.println(")");
            return false;
        }

        Config tmp;
        memset(&tmp, 0, sizeof(tmp));  // zero-fill so v1 migration gets clean defaults

        if (is_v1) {
            // v1 layout is different from v2: display_name sits right
            // after lxmf_interval_ms (no bt_pin/lat/lon/alt fields).
            // We can't memcpy directly into a v2 struct, so we parse
            // field-by-field from the raw bytes.
            const uint8_t* p = data.data();

            // Verify v1 CRC first (covers all bytes except trailing 4).
            const size_t v1_crc_covered = CONFIG_V1_SIZE - sizeof(uint32_t);
            uint32_t want;
            memcpy(&want, p + v1_crc_covered, sizeof(uint32_t));
            uint32_t actual = crc32_of(p, v1_crc_covered);
            if (actual != want) {
                Serial.print("Config: v1 CRC mismatch (got 0x");
                Serial.print(actual, HEX);
                Serial.print(", want 0x");
                Serial.print(want, HEX);
                Serial.println(")");
                return false;
            }

            // Manually unpack v1 fields (all packed, no padding).
            size_t off = 0;
            memcpy(&tmp.version,          p + off, 2); off += 2;
            memcpy(&tmp._reserved,        p + off, 2); off += 2;
            memcpy(&tmp.freq_hz,          p + off, 4); off += 4;
            memcpy(&tmp.bw_hz,            p + off, 4); off += 4;
            memcpy(&tmp.sf,               p + off, 1); off += 1;
            memcpy(&tmp.cr,               p + off, 1); off += 1;
            memcpy(&tmp.txp_dbm,          p + off, 1); off += 1;
            memcpy(&tmp.flags,            p + off, 1); off += 1;
            memcpy(&tmp.batt_mult,        p + off, 4); off += 4;
            memcpy(&tmp.tele_interval_ms, p + off, 4); off += 4;
            memcpy(&tmp.lxmf_interval_ms, p + off, 4); off += 4;
            // In v1 display_name is next (no bt/location fields)
            memcpy(&tmp.display_name,     p + off, 32); off += 32;
            // off == 60, then crc32 at 60..63 (already verified)

            // Validate the v1 fields
            if (!validate(tmp)) {
                Serial.println("Config: v1 CRC OK but field validation failed");
                return false;
            }

            // Migrate straight to v3: zero the v2 (bt/location) and v3
            // (collector) additions, bump version. tmp was memset(0) above
            // so collector_hash and the bt/location fields are already 0.
            tmp.bt_pin         = 0;
            tmp.latitude_udeg  = 0;
            tmp.longitude_udeg = 0;
            tmp.altitude_m     = 0;
            tmp.flags         &= ~CONFIG_FLAG_BT_ENABLED;  // BT off by default
            tmp.version        = 3;

            // Re-compute CRC for the v3 layout and persist so future
            // boots take the fast path.
            tmp.crc32 = 0;
            const size_t v3_crc_covered = sizeof(Config) - sizeof(uint32_t);
            tmp.crc32 = crc32_of(reinterpret_cast<const uint8_t*>(&tmp), v3_crc_covered);

            RNS::Bytes migrated(reinterpret_cast<const uint8_t*>(&tmp), sizeof(Config));
            RNS::Utilities::OS::write_file(CONFIG_PATH, migrated);
            Serial.println("Config: migrated v1 -> v3");

            out = tmp;
            return true;
        }

        if (is_v2) {
            // v2 -> v3 migration. v2 shares the first CONFIG_SHARED_PREFIX
            // (76) bytes with v3, then has crc32; v3 inserts
            // collector_hash[16] before crc32. Verify the v2 CRC (covers
            // the 76-byte prefix), copy that prefix into the v3 struct,
            // leave collector_hash zeroed, re-stamp a v3 CRC, persist.
            const uint8_t* p = data.data();

            uint32_t want;
            memcpy(&want, p + CONFIG_SHARED_PREFIX, sizeof(uint32_t));
            uint32_t actual = crc32_of(p, CONFIG_SHARED_PREFIX);
            if (actual != want) {
                Serial.print("Config: v2 CRC mismatch (got 0x");
                Serial.print(actual, HEX);
                Serial.print(", want 0x");
                Serial.print(want, HEX);
                Serial.println(")");
                return false;
            }

            memcpy(&tmp, p, CONFIG_SHARED_PREFIX);   // version..display_name
            // collector_hash already zeroed by the memset above.
            tmp.version = 3;

            if (!validate(tmp)) {
                Serial.println("Config: v2 CRC OK but field validation failed");
                return false;
            }

            tmp.crc32 = 0;
            const size_t v3_crc_covered = sizeof(Config) - sizeof(uint32_t);
            tmp.crc32 = crc32_of(reinterpret_cast<const uint8_t*>(&tmp), v3_crc_covered);

            RNS::Bytes migrated(reinterpret_cast<const uint8_t*>(&tmp), sizeof(Config));
            RNS::Utilities::OS::write_file(CONFIG_PATH, migrated);
            Serial.println("Config: migrated v2 -> v3");

            out = tmp;
            return true;
        }

        // Normal v3 load path
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
        tmp.version = 3;
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
    Serial.println("Config: defaults loaded (run CONFIG COMMIT to persist)");
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

// Single hex digit to value, or -1 if not a hex character.
static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

const char* set_field(Config& cfg, const char* key, const char* value) {
    if (!key || !value) return "missing key or value";

    if (streq(key, "display_name")) {
        size_t n = strlen(value);
        if (n == 0)                          return "display_name must not be empty";
        if (n >= sizeof(cfg.display_name))   return "display_name max 31 chars";
        for (size_t i = 0; i < n; i++) {
            if (value[i] == '|')             return "display_name must not contain '|'";
        }
        memset(cfg.display_name, 0, sizeof(cfg.display_name));
        memcpy(cfg.display_name, value, n);
        return nullptr;
    }
    if (streq(key, "freq_hz")) {
        char* end = nullptr;
        unsigned long v = strtoul(value, &end, 10);
        if (end == value || *end != '\0')    return "freq_hz must be an integer";
        if (v < 100000000UL || v > 1100000000UL) return "freq_hz range 100000000..1100000000";
        cfg.freq_hz = (uint32_t)v;
        return nullptr;
    }
    if (streq(key, "bw_hz")) {
        char* end = nullptr;
        unsigned long v = strtoul(value, &end, 10);
        if (end == value || *end != '\0')    return "bw_hz must be an integer";
        if (v < 7800UL || v > 500000UL)     return "bw_hz range 7800..500000";
        cfg.bw_hz = (uint32_t)v;
        return nullptr;
    }
    if (streq(key, "sf")) {
        char* end = nullptr;
        long v = strtol(value, &end, 10);
        if (end == value || *end != '\0')    return "sf must be an integer";
        if (v < 7 || v > 12)                return "sf range 7..12";
        cfg.sf = (uint8_t)v;
        return nullptr;
    }
    if (streq(key, "cr")) {
        char* end = nullptr;
        long v = strtol(value, &end, 10);
        if (end == value || *end != '\0')    return "cr must be an integer";
        if (v < 5 || v > 8)                 return "cr range 5..8";
        cfg.cr = (uint8_t)v;
        return nullptr;
    }
    if (streq(key, "txp_dbm")) {
        char* end = nullptr;
        long v = strtol(value, &end, 10);
        if (end == value || *end != '\0')    return "txp_dbm must be an integer";
        if (v < -9 || v > 22)               return "txp_dbm range -9..22";
        cfg.txp_dbm = (int8_t)v;
        return nullptr;
    }
    if (streq(key, "batt_mult")) {
        char* end = nullptr;
        float v = strtof(value, &end);
        if (end == value || *end != '\0')    return "batt_mult must be a number";
        if (v <= 0.0f || v > 10.0f)         return "batt_mult range 0.01..10.0";
        cfg.batt_mult = v;
        return nullptr;
    }
    if (streq(key, "tele_interval_ms")) {
        char* end = nullptr;
        unsigned long v = strtoul(value, &end, 10);
        if (end == value || *end != '\0')    return "tele_interval_ms must be an integer";
        cfg.tele_interval_ms = (uint32_t)v;
        return nullptr;
    }
    if (streq(key, "lxmf_interval_ms")) {
        char* end = nullptr;
        unsigned long v = strtoul(value, &end, 10);
        if (end == value || *end != '\0')    return "lxmf_interval_ms must be an integer";
        cfg.lxmf_interval_ms = (uint32_t)v;
        return nullptr;
    }
    if (streq(key, "telemetry") || streq(key, "lxmf") || streq(key, "heartbeat") || streq(key, "bt_enabled")) {
        bool b;
        if (!parse_bool(value, b))           return "expected 0/1, true/false, on/off, yes/no";
        uint8_t bit = streq(key, "telemetry")  ? CONFIG_FLAG_TELEMETRY
                    : streq(key, "lxmf")       ? CONFIG_FLAG_LXMF
                    : streq(key, "heartbeat")  ? CONFIG_FLAG_HEARTBEAT
                                               : CONFIG_FLAG_BT_ENABLED;
        if (b) cfg.flags |=  bit;
        else   cfg.flags &= ~bit;
        return nullptr;
    }
    if (streq(key, "bt_pin")) {
        char* end = nullptr;
        unsigned long v = strtoul(value, &end, 10);
        if (end == value || *end != '\0')    return "bt_pin must be an integer";
        if (v > 999999)                      return "bt_pin range 0..999999";
        cfg.bt_pin = (uint32_t)v;
        return nullptr;
    }
    if (streq(key, "latitude")) {
        char* end = nullptr;
        double v = strtod(value, &end);
        if (end == value || *end != '\0')    return "latitude must be a number";
        int32_t udeg = (int32_t)(v * 1000000.0);
        if (udeg < -90000000 || udeg > 90000000) return "latitude range -90..90";
        cfg.latitude_udeg = udeg;
        return nullptr;
    }
    if (streq(key, "longitude")) {
        char* end = nullptr;
        double v = strtod(value, &end);
        if (end == value || *end != '\0')    return "longitude must be a number";
        int32_t udeg = (int32_t)(v * 1000000.0);
        if (udeg < -180000000 || udeg > 180000000) return "longitude range -180..180";
        cfg.longitude_udeg = udeg;
        return nullptr;
    }
    if (streq(key, "altitude")) {
        char* end = nullptr;
        long v = strtol(value, &end, 10);
        if (end == value || *end != '\0')    return "altitude must be an integer";
        if (v < -100000 || v > 100000)       return "altitude range -100000..100000";
        cfg.altitude_m = (int32_t)v;
        return nullptr;
    }
    if (streq(key, "log_level")) {
        char* end = nullptr;
        long v = strtol(value, &end, 10);
        if (end == value || *end != '\0')    return "log_level must be an integer";
        if (v < 0 || v > 2)                 return "log_level range 0..2 (0=quiet, 1=normal, 2=verbose)";
        cfg.log_level = (uint8_t)v;
        return nullptr;
    }
    if (streq(key, "collector")) {
        // The telemetry collector's 16-byte lxmf.delivery destination
        // hash, as 32 hex chars. "none"/"off"/"clear"/empty disables
        // telemetry pushes (all-zero hash).
        if (value[0] == '\0' || streq(value, "none") || streq(value, "off") || streq(value, "clear")) {
            memset(cfg.collector_hash, 0, sizeof(cfg.collector_hash));
            return nullptr;
        }
        if (strlen(value) != 32)             return "collector must be 32 hex chars (16-byte dest hash) or 'none'";
        uint8_t tmp[16];
        for (size_t i = 0; i < 16; i++) {
            int hi = hexval(value[2*i]);
            int lo = hexval(value[2*i + 1]);
            if (hi < 0 || lo < 0)            return "collector must be hexadecimal";
            tmp[i] = (uint8_t)((hi << 4) | lo);
        }
        memcpy(cfg.collector_hash, tmp, sizeof(cfg.collector_hash));
        return nullptr;
    }
    return "unknown key";
}

// Print the collector destination hash as lowercase hex, or nothing if
// it's unset (all-zero) — so an empty field round-trips back through
// set_field("collector", "") as "clear".
static void print_collector_hex(const Config& cfg, Print& out) {
    bool all_zero = true;
    for (size_t i = 0; i < sizeof(cfg.collector_hash); i++) {
        if (cfg.collector_hash[i] != 0) { all_zero = false; break; }
    }
    if (all_zero) return;
    static const char* hx = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(cfg.collector_hash); i++) {
        out.print(hx[cfg.collector_hash[i] >> 4]);
        out.print(hx[cfg.collector_hash[i] & 0x0f]);
    }
}

void print_fields(const Config& cfg, Print& out) {
    out.print("display_name=");     out.println(cfg.display_name);
    out.print("freq_hz=");          out.println(cfg.freq_hz);
    out.print("bw_hz=");            out.println(cfg.bw_hz);
    out.print("sf=");               out.println(cfg.sf);
    out.print("cr=");               out.println(cfg.cr);
    out.print("txp_dbm=");          out.println(cfg.txp_dbm);
    out.print("batt_mult=");        out.println(cfg.batt_mult, 4);
    out.print("tele_interval_ms="); out.println(cfg.tele_interval_ms);
    out.print("lxmf_interval_ms="); out.println(cfg.lxmf_interval_ms);
    out.print("telemetry=");        out.println((cfg.flags & CONFIG_FLAG_TELEMETRY)  ? 1 : 0);
    out.print("lxmf=");             out.println((cfg.flags & CONFIG_FLAG_LXMF)       ? 1 : 0);
    out.print("heartbeat=");        out.println((cfg.flags & CONFIG_FLAG_HEARTBEAT)  ? 1 : 0);
    out.print("bt_enabled=");       out.println((cfg.flags & CONFIG_FLAG_BT_ENABLED) ? 1 : 0);
    out.print("bt_pin=");           out.println(cfg.bt_pin);
    out.print("latitude=");         out.println(cfg.latitude_udeg  / 1000000.0, 6);
    out.print("longitude=");        out.println(cfg.longitude_udeg / 1000000.0, 6);
    out.print("altitude=");         out.println(cfg.altitude_m);
    out.print("log_level=");       out.println(cfg.log_level);
    out.print("collector=");        print_collector_hex(cfg, out); out.println();
}

void print_fields_pipe(const Config& cfg, Print& out) {
    // Pipe-delimited single line using Print methods (no snprintf).
    // Arduino's Print::print(float) handles float formatting reliably
    // on ARM newlib where snprintf %f can fail silently.
    //
    // Field order: display_name|freq_hz|bw_hz|sf|cr|txp_dbm|batt_mult|
    //   tele_interval_ms|lxmf_interval_ms|telemetry|lxmf|heartbeat|
    //   bt_enabled|bt_pin|latitude|longitude|altitude|log_level|collector
    // Must stay in lockstep with Ble.cpp keys[] and console.js PIPE_FIELDS.
    out.print(cfg.display_name);       out.print('|');
    out.print(cfg.freq_hz);            out.print('|');
    out.print(cfg.bw_hz);              out.print('|');
    out.print(cfg.sf);                 out.print('|');
    out.print(cfg.cr);                 out.print('|');
    out.print(cfg.txp_dbm);           out.print('|');
    out.print(cfg.batt_mult, 4);       out.print('|');
    out.print(cfg.tele_interval_ms);   out.print('|');
    out.print(cfg.lxmf_interval_ms);   out.print('|');
    out.print((cfg.flags & CONFIG_FLAG_TELEMETRY)  ? 1 : 0); out.print('|');
    out.print((cfg.flags & CONFIG_FLAG_LXMF)       ? 1 : 0); out.print('|');
    out.print((cfg.flags & CONFIG_FLAG_HEARTBEAT)  ? 1 : 0); out.print('|');
    out.print((cfg.flags & CONFIG_FLAG_BT_ENABLED) ? 1 : 0); out.print('|');
    out.print(cfg.bt_pin);             out.print('|');
    out.print(cfg.latitude_udeg  / 1000000.0, 6); out.print('|');
    out.print(cfg.longitude_udeg / 1000000.0, 6); out.print('|');
    out.print(cfg.altitude_m);         out.print('|');
    out.print(cfg.log_level);          out.print('|');
    print_collector_hex(cfg, out);
    // No println — caller may append more fields before the line ends.
}

}} // namespace rlr::config
