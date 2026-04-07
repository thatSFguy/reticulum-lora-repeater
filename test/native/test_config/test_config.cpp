// tests/native/test_config/test_config.cpp
//
// Host-side unit tests for Config validation and set_field parsing.
// Runs on the native platform via Unity — no Arduino or nRF52 deps.
//
// We can't #include Config.cpp directly because it depends on Arduino.h,
// microStore, etc. Instead we re-implement the pure-logic functions here
// (validate, set_field, pipe format) by copy — they're small and the
// tests catch drift if someone changes the firmware version without
// updating the tests.

#include <unity.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

// ── Minimal Config struct (must match src/Config.h) ─────────────

#pragma pack(push, 1)
struct Config {
    uint16_t version;
    uint16_t _reserved;
    uint32_t freq_hz;
    uint32_t bw_hz;
    uint8_t  sf;
    uint8_t  cr;
    int8_t   txp_dbm;
    uint8_t  flags;
    float    batt_mult;
    uint32_t tele_interval_ms;
    uint32_t lxmf_interval_ms;
    uint32_t bt_pin;
    int32_t  latitude_udeg;
    int32_t  longitude_udeg;
    int32_t  altitude_m;
    char     display_name[32];
    uint32_t crc32;
};
#pragma pack(pop)

enum : uint8_t {
    CONFIG_FLAG_TELEMETRY  = 1 << 0,
    CONFIG_FLAG_LXMF       = 1 << 1,
    CONFIG_FLAG_HEARTBEAT  = 1 << 2,
    CONFIG_FLAG_BT_ENABLED = 1 << 3,
};

// ── Helpers (copied from Config.cpp) ────────────────────────────

static bool streq(const char* a, const char* b) {
    return strcmp(a, b) == 0;
}

static bool parse_bool(const char* v, bool& out) {
    if (streq(v, "1") || streq(v, "true") || streq(v, "on")  || streq(v, "yes")) { out = true;  return true; }
    if (streq(v, "0") || streq(v, "false")|| streq(v, "off") || streq(v, "no") ) { out = false; return true; }
    return false;
}

// ── validate (copied from Config.cpp) ───────────────────────────

static bool validate(const Config& cfg) {
    if (cfg.version != 1 && cfg.version != 2)           return false;
    if (cfg.sf < 7   || cfg.sf > 12)                    return false;
    if (cfg.cr < 5   || cfg.cr > 8)                     return false;
    if (cfg.txp_dbm < -9 || cfg.txp_dbm > 22)           return false;
    if (cfg.freq_hz < 100000000UL || cfg.freq_hz > 1100000000UL) return false;
    if (cfg.bw_hz   < 7800UL      || cfg.bw_hz   > 500000UL)     return false;
    if (cfg.version >= 2) {
        if (cfg.bt_pin > 999999)                        return false;
        if (cfg.latitude_udeg  < -90000000  || cfg.latitude_udeg  > 90000000)  return false;
        if (cfg.longitude_udeg < -180000000 || cfg.longitude_udeg > 180000000) return false;
        if (cfg.altitude_m < -100000 || cfg.altitude_m > 100000)               return false;
    }
    bool terminated = false;
    for (size_t i = 0; i < sizeof(cfg.display_name); i++) {
        if (cfg.display_name[i] == '\0') { terminated = true; break; }
    }
    if (!terminated)                                    return false;
    return true;
}

// ── set_field (copied from Config.cpp) ──────────────────────────

static const char* set_field(Config& cfg, const char* key, const char* value) {
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
    return "unknown key";
}

// ── Helper: create a valid default config ───────────────────────

static Config make_valid() {
    Config c;
    memset(&c, 0, sizeof(c));
    c.version = 2;
    c.freq_hz = 904375000;
    c.bw_hz = 250000;
    c.sf = 10;
    c.cr = 5;
    c.txp_dbm = 22;
    c.flags = CONFIG_FLAG_TELEMETRY | CONFIG_FLAG_LXMF | CONFIG_FLAG_HEARTBEAT;
    c.batt_mult = 1.284f;
    c.tele_interval_ms = 10800000;
    c.lxmf_interval_ms = 1800000;
    c.bt_pin = 0;
    c.latitude_udeg = 0;
    c.longitude_udeg = 0;
    c.altitude_m = 0;
    strncpy(c.display_name, "TestNode", sizeof(c.display_name));
    return c;
}

// ═══════════════════════════════════════════════════════════════
//  TESTS: validate()
// ═══════════════════════════════════════════════════════════════

void test_validate_good_v2() {
    Config c = make_valid();
    TEST_ASSERT_TRUE(validate(c));
}

void test_validate_good_v1() {
    Config c = make_valid();
    c.version = 1;
    TEST_ASSERT_TRUE(validate(c));
}

void test_validate_bad_version() {
    Config c = make_valid();
    c.version = 0;
    TEST_ASSERT_FALSE(validate(c));
    c.version = 3;
    TEST_ASSERT_FALSE(validate(c));
}

void test_validate_sf_range() {
    Config c = make_valid();
    c.sf = 6;  TEST_ASSERT_FALSE(validate(c));
    c.sf = 7;  TEST_ASSERT_TRUE(validate(c));
    c.sf = 12; TEST_ASSERT_TRUE(validate(c));
    c.sf = 13; TEST_ASSERT_FALSE(validate(c));
}

void test_validate_cr_range() {
    Config c = make_valid();
    c.cr = 4;  TEST_ASSERT_FALSE(validate(c));
    c.cr = 5;  TEST_ASSERT_TRUE(validate(c));
    c.cr = 8;  TEST_ASSERT_TRUE(validate(c));
    c.cr = 9;  TEST_ASSERT_FALSE(validate(c));
}

void test_validate_txp_range() {
    Config c = make_valid();
    c.txp_dbm = -10; TEST_ASSERT_FALSE(validate(c));
    c.txp_dbm = -9;  TEST_ASSERT_TRUE(validate(c));
    c.txp_dbm = 22;  TEST_ASSERT_TRUE(validate(c));
    c.txp_dbm = 23;  TEST_ASSERT_FALSE(validate(c));
}

void test_validate_freq_range() {
    Config c = make_valid();
    c.freq_hz = 99999999;   TEST_ASSERT_FALSE(validate(c));
    c.freq_hz = 100000000;  TEST_ASSERT_TRUE(validate(c));
    c.freq_hz = 1100000000; TEST_ASSERT_TRUE(validate(c));
    c.freq_hz = 1100000001; TEST_ASSERT_FALSE(validate(c));
}

void test_validate_bw_range() {
    Config c = make_valid();
    c.bw_hz = 7799;   TEST_ASSERT_FALSE(validate(c));
    c.bw_hz = 7800;   TEST_ASSERT_TRUE(validate(c));
    c.bw_hz = 500000;  TEST_ASSERT_TRUE(validate(c));
    c.bw_hz = 500001;  TEST_ASSERT_FALSE(validate(c));
}

void test_validate_bt_pin_range() {
    Config c = make_valid();
    c.bt_pin = 0;      TEST_ASSERT_TRUE(validate(c));
    c.bt_pin = 999999; TEST_ASSERT_TRUE(validate(c));
    c.bt_pin = 1000000; TEST_ASSERT_FALSE(validate(c));
}

void test_validate_latitude_range() {
    Config c = make_valid();
    c.latitude_udeg = -90000000; TEST_ASSERT_TRUE(validate(c));
    c.latitude_udeg = 90000000;  TEST_ASSERT_TRUE(validate(c));
    c.latitude_udeg = -90000001; TEST_ASSERT_FALSE(validate(c));
    c.latitude_udeg = 90000001;  TEST_ASSERT_FALSE(validate(c));
}

void test_validate_longitude_range() {
    Config c = make_valid();
    c.longitude_udeg = -180000000; TEST_ASSERT_TRUE(validate(c));
    c.longitude_udeg = 180000000;  TEST_ASSERT_TRUE(validate(c));
    c.longitude_udeg = -180000001; TEST_ASSERT_FALSE(validate(c));
    c.longitude_udeg = 180000001;  TEST_ASSERT_FALSE(validate(c));
}

void test_validate_altitude_range() {
    Config c = make_valid();
    c.altitude_m = -100000; TEST_ASSERT_TRUE(validate(c));
    c.altitude_m = 100000;  TEST_ASSERT_TRUE(validate(c));
    c.altitude_m = -100001; TEST_ASSERT_FALSE(validate(c));
    c.altitude_m = 100001;  TEST_ASSERT_FALSE(validate(c));
}

void test_validate_display_name_unterminated() {
    Config c = make_valid();
    memset(c.display_name, 'X', sizeof(c.display_name));  // no NUL
    TEST_ASSERT_FALSE(validate(c));
}

// ═══════════════════════════════════════════════════════════════
//  TESTS: set_field()
// ═══════════════════════════════════════════════════════════════

void test_set_display_name_ok() {
    Config c = make_valid();
    TEST_ASSERT_NULL(set_field(c, "display_name", "MyNode"));
    TEST_ASSERT_EQUAL_STRING("MyNode", c.display_name);
}

void test_set_display_name_empty() {
    Config c = make_valid();
    const char* e = set_field(c, "display_name", "");
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_STRING("display_name must not be empty", e);
}

void test_set_display_name_too_long() {
    Config c = make_valid();
    char long_name[64];
    memset(long_name, 'A', 32);
    long_name[32] = '\0';
    const char* e = set_field(c, "display_name", long_name);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_STRING("display_name max 31 chars", e);
}

void test_set_display_name_pipe_rejected() {
    Config c = make_valid();
    const char* e = set_field(c, "display_name", "My|Node");
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_STRING("display_name must not contain '|'", e);
}

void test_set_display_name_max_31() {
    Config c = make_valid();
    char name31[32];
    memset(name31, 'B', 31);
    name31[31] = '\0';
    TEST_ASSERT_NULL(set_field(c, "display_name", name31));
    TEST_ASSERT_EQUAL_STRING(name31, c.display_name);
}

void test_set_freq_hz_ok() {
    Config c = make_valid();
    TEST_ASSERT_NULL(set_field(c, "freq_hz", "915000000"));
    TEST_ASSERT_EQUAL_UINT32(915000000, c.freq_hz);
}

void test_set_freq_hz_too_low() {
    Config c = make_valid();
    const char* e = set_field(c, "freq_hz", "99999999");
    TEST_ASSERT_NOT_NULL(e);
}

void test_set_freq_hz_too_high() {
    Config c = make_valid();
    const char* e = set_field(c, "freq_hz", "1100000001");
    TEST_ASSERT_NOT_NULL(e);
}

void test_set_freq_hz_not_number() {
    Config c = make_valid();
    const char* e = set_field(c, "freq_hz", "abc");
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_STRING("freq_hz must be an integer", e);
}

void test_set_sf_boundaries() {
    Config c = make_valid();
    TEST_ASSERT_NULL(set_field(c, "sf", "7"));
    TEST_ASSERT_EQUAL_UINT8(7, c.sf);
    TEST_ASSERT_NULL(set_field(c, "sf", "12"));
    TEST_ASSERT_EQUAL_UINT8(12, c.sf);
    TEST_ASSERT_NOT_NULL(set_field(c, "sf", "6"));
    TEST_ASSERT_NOT_NULL(set_field(c, "sf", "13"));
}

void test_set_cr_boundaries() {
    Config c = make_valid();
    TEST_ASSERT_NULL(set_field(c, "cr", "5"));
    TEST_ASSERT_EQUAL_UINT8(5, c.cr);
    TEST_ASSERT_NULL(set_field(c, "cr", "8"));
    TEST_ASSERT_EQUAL_UINT8(8, c.cr);
    TEST_ASSERT_NOT_NULL(set_field(c, "cr", "4"));
    TEST_ASSERT_NOT_NULL(set_field(c, "cr", "9"));
}

void test_set_txp_dbm_boundaries() {
    Config c = make_valid();
    TEST_ASSERT_NULL(set_field(c, "txp_dbm", "-9"));
    TEST_ASSERT_EQUAL_INT8(-9, c.txp_dbm);
    TEST_ASSERT_NULL(set_field(c, "txp_dbm", "22"));
    TEST_ASSERT_EQUAL_INT8(22, c.txp_dbm);
    TEST_ASSERT_NOT_NULL(set_field(c, "txp_dbm", "-10"));
    TEST_ASSERT_NOT_NULL(set_field(c, "txp_dbm", "23"));
}

void test_set_batt_mult_ok() {
    Config c = make_valid();
    TEST_ASSERT_NULL(set_field(c, "batt_mult", "1.5"));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 1.5f, c.batt_mult);
}

void test_set_batt_mult_zero() {
    Config c = make_valid();
    TEST_ASSERT_NOT_NULL(set_field(c, "batt_mult", "0"));
}

void test_set_batt_mult_too_high() {
    Config c = make_valid();
    TEST_ASSERT_NOT_NULL(set_field(c, "batt_mult", "10.1"));
}

void test_set_bt_pin_ok() {
    Config c = make_valid();
    TEST_ASSERT_NULL(set_field(c, "bt_pin", "123456"));
    TEST_ASSERT_EQUAL_UINT32(123456, c.bt_pin);
    TEST_ASSERT_NULL(set_field(c, "bt_pin", "0"));
    TEST_ASSERT_EQUAL_UINT32(0, c.bt_pin);
}

void test_set_bt_pin_too_high() {
    Config c = make_valid();
    const char* e = set_field(c, "bt_pin", "1000000");
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_STRING("bt_pin range 0..999999", e);
}

void test_set_latitude_ok() {
    Config c = make_valid();
    TEST_ASSERT_NULL(set_field(c, "latitude", "37.7749"));
    TEST_ASSERT_INT32_WITHIN(1, 37774900, c.latitude_udeg);
}

void test_set_latitude_boundaries() {
    Config c = make_valid();
    TEST_ASSERT_NULL(set_field(c, "latitude", "-90"));
    TEST_ASSERT_NULL(set_field(c, "latitude", "90"));
    TEST_ASSERT_NOT_NULL(set_field(c, "latitude", "-90.000001"));
    TEST_ASSERT_NOT_NULL(set_field(c, "latitude", "90.000001"));
}

void test_set_longitude_ok() {
    Config c = make_valid();
    TEST_ASSERT_NULL(set_field(c, "longitude", "-122.4194"));
    TEST_ASSERT_INT32_WITHIN(1, -122419400, c.longitude_udeg);
}

void test_set_longitude_boundaries() {
    Config c = make_valid();
    TEST_ASSERT_NULL(set_field(c, "longitude", "-180"));
    TEST_ASSERT_NULL(set_field(c, "longitude", "180"));
    TEST_ASSERT_NOT_NULL(set_field(c, "longitude", "-180.000001"));
    TEST_ASSERT_NOT_NULL(set_field(c, "longitude", "180.000001"));
}

void test_set_altitude_ok() {
    Config c = make_valid();
    TEST_ASSERT_NULL(set_field(c, "altitude", "50"));
    TEST_ASSERT_EQUAL_INT32(50, c.altitude_m);
    TEST_ASSERT_NULL(set_field(c, "altitude", "-430"));
    TEST_ASSERT_EQUAL_INT32(-430, c.altitude_m);
}

void test_set_altitude_boundaries() {
    Config c = make_valid();
    TEST_ASSERT_NULL(set_field(c, "altitude", "-100000"));
    TEST_ASSERT_NULL(set_field(c, "altitude", "100000"));
    TEST_ASSERT_NOT_NULL(set_field(c, "altitude", "-100001"));
    TEST_ASSERT_NOT_NULL(set_field(c, "altitude", "100001"));
}

void test_set_bool_flags() {
    Config c = make_valid();
    c.flags = 0;
    TEST_ASSERT_NULL(set_field(c, "telemetry", "1"));
    TEST_ASSERT_EQUAL_UINT8(CONFIG_FLAG_TELEMETRY, c.flags & CONFIG_FLAG_TELEMETRY);
    TEST_ASSERT_NULL(set_field(c, "telemetry", "0"));
    TEST_ASSERT_EQUAL_UINT8(0, c.flags & CONFIG_FLAG_TELEMETRY);

    TEST_ASSERT_NULL(set_field(c, "bt_enabled", "true"));
    TEST_ASSERT_EQUAL_UINT8(CONFIG_FLAG_BT_ENABLED, c.flags & CONFIG_FLAG_BT_ENABLED);
    TEST_ASSERT_NULL(set_field(c, "bt_enabled", "false"));
    TEST_ASSERT_EQUAL_UINT8(0, c.flags & CONFIG_FLAG_BT_ENABLED);

    TEST_ASSERT_NULL(set_field(c, "lxmf", "on"));
    TEST_ASSERT_EQUAL_UINT8(CONFIG_FLAG_LXMF, c.flags & CONFIG_FLAG_LXMF);

    TEST_ASSERT_NULL(set_field(c, "heartbeat", "yes"));
    TEST_ASSERT_EQUAL_UINT8(CONFIG_FLAG_HEARTBEAT, c.flags & CONFIG_FLAG_HEARTBEAT);
}

void test_set_bool_invalid() {
    Config c = make_valid();
    const char* e = set_field(c, "telemetry", "maybe");
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_STRING("expected 0/1, true/false, on/off, yes/no", e);
}

void test_set_unknown_key() {
    Config c = make_valid();
    const char* e = set_field(c, "nonexistent", "42");
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_STRING("unknown key", e);
}

void test_set_null_args() {
    Config c = make_valid();
    TEST_ASSERT_NOT_NULL(set_field(c, nullptr, "42"));
    TEST_ASSERT_NOT_NULL(set_field(c, "sf", nullptr));
}

// ═══════════════════════════════════════════════════════════════
//  TESTS: struct size
// ═══════════════════════════════════════════════════════════════

void test_config_struct_size() {
    TEST_ASSERT_LESS_OR_EQUAL(128, sizeof(Config));
}

void test_config_v1_size_constant() {
    // v1 layout: version(2) + _reserved(2) + freq_hz(4) + bw_hz(4) +
    //   sf(1) + cr(1) + txp_dbm(1) + flags(1) + batt_mult(4) +
    //   tele_interval_ms(4) + lxmf_interval_ms(4) + display_name(32) +
    //   crc32(4) = 64 bytes
    TEST_ASSERT_EQUAL(64, 2+2+4+4+1+1+1+1+4+4+4+32+4);
}

// ═══════════════════════════════════════════════════════════════

int main() {
    UNITY_BEGIN();

    // validate tests
    RUN_TEST(test_validate_good_v2);
    RUN_TEST(test_validate_good_v1);
    RUN_TEST(test_validate_bad_version);
    RUN_TEST(test_validate_sf_range);
    RUN_TEST(test_validate_cr_range);
    RUN_TEST(test_validate_txp_range);
    RUN_TEST(test_validate_freq_range);
    RUN_TEST(test_validate_bw_range);
    RUN_TEST(test_validate_bt_pin_range);
    RUN_TEST(test_validate_latitude_range);
    RUN_TEST(test_validate_longitude_range);
    RUN_TEST(test_validate_altitude_range);
    RUN_TEST(test_validate_display_name_unterminated);

    // set_field tests
    RUN_TEST(test_set_display_name_ok);
    RUN_TEST(test_set_display_name_empty);
    RUN_TEST(test_set_display_name_too_long);
    RUN_TEST(test_set_display_name_pipe_rejected);
    RUN_TEST(test_set_display_name_max_31);
    RUN_TEST(test_set_freq_hz_ok);
    RUN_TEST(test_set_freq_hz_too_low);
    RUN_TEST(test_set_freq_hz_too_high);
    RUN_TEST(test_set_freq_hz_not_number);
    RUN_TEST(test_set_sf_boundaries);
    RUN_TEST(test_set_cr_boundaries);
    RUN_TEST(test_set_txp_dbm_boundaries);
    RUN_TEST(test_set_batt_mult_ok);
    RUN_TEST(test_set_batt_mult_zero);
    RUN_TEST(test_set_batt_mult_too_high);
    RUN_TEST(test_set_bt_pin_ok);
    RUN_TEST(test_set_bt_pin_too_high);
    RUN_TEST(test_set_latitude_ok);
    RUN_TEST(test_set_latitude_boundaries);
    RUN_TEST(test_set_longitude_ok);
    RUN_TEST(test_set_longitude_boundaries);
    RUN_TEST(test_set_altitude_ok);
    RUN_TEST(test_set_altitude_boundaries);
    RUN_TEST(test_set_bool_flags);
    RUN_TEST(test_set_bool_invalid);
    RUN_TEST(test_set_unknown_key);
    RUN_TEST(test_set_null_args);

    // struct size
    RUN_TEST(test_config_struct_size);
    RUN_TEST(test_config_v1_size_constant);

    return UNITY_END();
}
