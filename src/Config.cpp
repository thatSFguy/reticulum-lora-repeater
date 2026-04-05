// src/Config.cpp — stub. Phase 3 implements microStore load/save.
#include "Config.h"
#include <string.h>

namespace rlr { namespace config {

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
    strncpy(out.display_name, DEFAULT_CONFIG_DISPLAY_NAME, sizeof(out.display_name) - 1);
    out.crc32 = 0;  // populated on save
}

bool load(Config& out) {
    // TODO: Phase 3 — read from microStore key "config"
    defaults(out);
    return false;
}

bool save(const Config& in) {
    // TODO: Phase 3 — compute CRC, write to microStore
    (void)in;
    return false;
}

void load_or_defaults(Config& out) {
    if (!load(out)) {
        defaults(out);
    }
}

bool validate(const Config& cfg) {
    if (cfg.version != 1) return false;
    if (cfg.sf < 7 || cfg.sf > 12) return false;
    if (cfg.cr < 5 || cfg.cr > 8) return false;
    if (cfg.txp_dbm < -9 || cfg.txp_dbm > 22) return false;
    if (cfg.freq_hz < 100000000UL || cfg.freq_hz > 1100000000UL) return false;
    if (cfg.bw_hz < 7800UL || cfg.bw_hz > 500000UL) return false;
    return true;
}

bool set_field(Config& cfg, const char* key, const char* value) {
    // TODO: Phase 4 — implement key dispatch for the serial console
    (void)cfg; (void)key; (void)value;
    return false;
}

void print_fields(const Config& cfg) {
    // TODO: Phase 4 — write "key=value" lines to Serial
    (void)cfg;
}

}} // namespace rlr::config
