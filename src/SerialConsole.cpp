// src/SerialConsole.cpp — Phase 4 provisioning console.
//
// A tiny line-oriented command protocol spoken over USB CDC so the
// webflasher (and any plain serial terminal) can read status and edit
// the persistent Config without a rebuild. Protocol rules:
//
//   - One command per line, CR and/or LF terminated.
//   - Every command emits zero or more payload lines followed by a
//     single terminator line: "OK" on success, "ERR: <reason>" on
//     failure. Clients read until they see one of those.
//   - Edits land in a staging Config. Nothing touches flash until
//     CONFIG COMMIT, which validates + persists + reboots.
//   - CONFIG REVERT discards staged edits by re-seeding staging from
//     the live in-memory Config main.cpp is running with.
//
// The line reader is intentionally minimal: a fixed buffer, no
// history, no escape processing. Anything that needs more structure
// belongs in the webflasher UI on the host side.

#include "SerialConsole.h"
#include "Transport.h"
#include "Radio.h"
#include "Telemetry.h"
#include "LxmfPresence.h"
#include "Led.h"

#include <Arduino.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#ifndef RLR_VERSION
  #define RLR_VERSION "0.1.0-dev"
#endif

namespace rlr { namespace serial_console {

// ---- module state ------------------------------------------------

static Config* s_live     = nullptr;     // pointer to main.cpp's g_config
static Config  s_staging{};              // user-editable copy
static char    s_line[192];
static size_t  s_len      = 0;

// ---- helpers -----------------------------------------------------

static void ok()                        { Serial.println("OK"); }
static void err(const char* reason)     { Serial.print("ERR: "); Serial.println(reason); }

// Uppercase-in-place for case-insensitive command matching. Only
// touches ASCII letters; values (e.g. display_name) are matched in a
// separate copy so user data keeps its case.
static void upper(char* s) {
    for (; *s; ++s) *s = (char)toupper((unsigned char)*s);
}

// Trim leading spaces, return pointer into the same buffer.
static char* ltrim(char* s) {
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

// Split "key value rest..." at the first run of whitespace. Returns
// a pointer to the value (possibly empty string if no value present)
// and NUL-terminates the key in place.
static char* split_kv(char* s) {
    char* p = s;
    while (*p && *p != ' ' && *p != '\t') p++;
    if (*p == '\0') return p;  // empty value
    *p++ = '\0';
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

// ---- command handlers --------------------------------------------

static void cmd_help() {
    Serial.println("Commands:");
    Serial.println("  PING                       - liveness check");
    Serial.println("  VERSION                    - firmware version");
    Serial.println("  STATUS                     - runtime status");
    Serial.println("  HELP                       - this help");
    Serial.println("  REBOOT                     - NVIC system reset");
    Serial.println("  CONFIG GET                 - print staged config");
    Serial.println("  CONFIG SET <key> <value>   - stage a field change");
    Serial.println("  CONFIG RESET               - reseed staging from defaults");
    Serial.println("  CONFIG REVERT              - reseed staging from live config");
    Serial.println("  CONFIG COMMIT              - persist staging + reboot");
    Serial.println("  CALIBRATE BATTERY <mv>     - derive batt_mult from measured voltage");
    Serial.println("  ANNOUNCE                   - force LXMF + telemetry announce now");
    ok();
}

static void cmd_version() {
    Serial.print("version=");
    Serial.println(RLR_VERSION);
    ok();
}

static void cmd_status() {
    uint32_t up_s = millis() / 1000;
    Serial.print("uptime_s=");  Serial.println(up_s);
    Serial.print("radio=");     Serial.println(rlr::radio::online() ? "up" : "down");
    Serial.print("packets_in=");  Serial.println(rlr::transport::packets_in());
    Serial.print("packets_out="); Serial.println(rlr::transport::packets_out());
    Serial.print("paths=");       Serial.println(rlr::transport::path_count());
    Serial.print("destinations="); Serial.println(rlr::transport::destination_count());
    if (s_live) {
        Serial.print("display_name="); Serial.println(s_live->display_name);
        // Raw + scaled battery readings so the webflasher (or a human)
        // can drive the CALIBRATE BATTERY workflow without guessing.
        uint32_t raw = rlr::telemetry::read_battery_raw_avg();
        Serial.print("battery_raw="); Serial.println(raw);
        Serial.print("battery_mv=");  Serial.println(rlr::telemetry::read_battery_mv(*s_live));
        Serial.print("batt_mult=");   Serial.println(s_live->batt_mult, 4);
    }
    ok();
}

// CALIBRATE BATTERY <measured_mv>
//
// One-step battery calibration: the user reports the voltage they
// see on a multimeter, firmware averages a fresh ADC burst, computes
// batt_mult = measured_mv / raw_avg, and stages it. The user still
// runs CONFIG COMMIT to persist — keeps the "nothing touches flash
// until you ask" invariant consistent with the rest of the console.
static void cmd_calibrate_battery(char* rest) {
    rest = ltrim(rest);
    if (*rest == '\0') { err("usage: CALIBRATE BATTERY <measured_mv>"); return; }
    char* end = nullptr;
    long measured = strtol(rest, &end, 10);
    if (end == rest || *end != '\0') { err("invalid mv"); return; }
    if (measured < 500 || measured > 10000) { err("mv out of range (500..10000)"); return; }

    uint32_t raw = rlr::telemetry::read_battery_raw_avg();
    if (raw == 0) { err("no battery ADC available or reading is 0"); return; }

    float mult = (float)measured / (float)raw;
    s_staging.batt_mult = mult;

    Serial.print("battery_raw="); Serial.println(raw);
    Serial.print("measured_mv="); Serial.println(measured);
    Serial.print("batt_mult=");   Serial.println(mult, 6);
    Serial.println("(staged — run CONFIG COMMIT to persist)");
    ok();
}

static void cmd_reboot() {
    Serial.println("rebooting...");
    ok();
    Serial.flush();
    delay(50);
    NVIC_SystemReset();
}

static void cmd_config_get() {
    config::print_fields(s_staging);
    ok();
}

static void cmd_config_set(char* rest) {
    rest = ltrim(rest);
    if (*rest == '\0') { err("usage: CONFIG SET <key> <value>"); return; }
    char* value = split_kv(rest);
    const char* key = rest;
    if (*value == '\0') { err("missing value"); return; }
    if (!config::set_field(s_staging, key, value)) {
        err("invalid key or value out of range");
        return;
    }
    ok();
}

static void cmd_config_reset() {
    config::defaults(s_staging);
    ok();
}

static void cmd_config_revert() {
    if (!s_live) { err("no live config"); return; }
    s_staging = *s_live;
    ok();
}

static void cmd_config_commit() {
    if (!config::validate(s_staging)) { err("staging validation failed"); return; }
    if (!config::save(s_staging))     { err("save failed");               return; }
    Serial.println("committed, rebooting...");
    ok();
    Serial.flush();
    delay(50);
    NVIC_SystemReset();
}

// ---- dispatcher --------------------------------------------------

static void dispatch(char* line) {
    // Strip trailing CR that may come in alongside LF from Windows.
    size_t n = strlen(line);
    while (n > 0 && (line[n-1] == '\r' || line[n-1] == '\n' || line[n-1] == ' ')) {
        line[--n] = '\0';
    }
    line = ltrim(line);
    if (*line == '\0') return;  // blank line — ignore silently

    // For dispatch we want a case-insensitive match on the command
    // keyword(s), but CONFIG SET's value half must preserve case
    // (e.g. display_name). So we uppercase a scratch copy for
    // matching and keep the original buffer intact for extracting
    // values when it's a CONFIG SET.
    char upper_copy[sizeof(s_line)];
    strncpy(upper_copy, line, sizeof(upper_copy));
    upper_copy[sizeof(upper_copy) - 1] = '\0';
    upper(upper_copy);

    if (strcmp(upper_copy, "PING") == 0)    { Serial.println("PONG"); ok(); return; }
    if (strcmp(upper_copy, "VERSION") == 0) { cmd_version(); return; }
    if (strcmp(upper_copy, "STATUS") == 0)  { cmd_status();  return; }
    if (strcmp(upper_copy, "HELP") == 0)    { cmd_help();    return; }
    if (strcmp(upper_copy, "REBOOT") == 0)  { cmd_reboot();  return; }

    // CONFIG subcommands. Match the uppercase prefix, then operate on
    // the original (case-preserving) buffer for any trailing args.
    if (strncmp(upper_copy, "CONFIG", 6) == 0 &&
        (upper_copy[6] == ' ' || upper_copy[6] == '\t')) {

        char* sub = ltrim(line + 6);
        char upper_sub[sizeof(s_line)];
        strncpy(upper_sub, sub, sizeof(upper_sub));
        upper_sub[sizeof(upper_sub) - 1] = '\0';
        upper(upper_sub);

        if (strcmp(upper_sub, "GET")    == 0) { cmd_config_get();    return; }
        if (strcmp(upper_sub, "RESET")  == 0) { cmd_config_reset();  return; }
        if (strcmp(upper_sub, "REVERT") == 0) { cmd_config_revert(); return; }
        if (strcmp(upper_sub, "COMMIT") == 0) { cmd_config_commit(); return; }
        if (strncmp(upper_sub, "SET", 3) == 0 &&
            (sub[3] == ' ' || sub[3] == '\t')) {
            cmd_config_set(sub + 4);
            return;
        }
        err("unknown CONFIG subcommand (try HELP)");
        return;
    }

    // ANNOUNCE — force immediate LXMF presence + telemetry announce
    if (strcmp(upper_copy, "ANNOUNCE") == 0) {
        if (!rlr::radio::online()) { err("radio not online"); return; }
        Serial.println("firing LXMF presence announce...");
        rlr::lxmf_presence::announce_now(*s_live);
        // Note: the telemetry announce may occasionally fail with a
        // transmit error when fired immediately after LXMF because
        // the radio is still transitioning from TX→RX. This is
        // harmless — automatic periodic announces are naturally
        // spaced and never hit this. We don't add a blocking delay
        // here because it would stall RX processing and tick().
        Serial.println("firing telemetry announce...");
        rlr::telemetry::announce_now(*s_live);
        ok();
        return;
    }

    // CALIBRATE BATTERY <mv>
    if (strncmp(upper_copy, "CALIBRATE", 9) == 0 &&
        (upper_copy[9] == ' ' || upper_copy[9] == '\t')) {

        char* sub = ltrim(line + 9);
        char upper_sub[sizeof(s_line)];
        strncpy(upper_sub, sub, sizeof(upper_sub));
        upper_sub[sizeof(upper_sub) - 1] = '\0';
        upper(upper_sub);

        if (strncmp(upper_sub, "BATTERY", 7) == 0 &&
            (sub[7] == ' ' || sub[7] == '\t')) {
            cmd_calibrate_battery(sub + 8);
            return;
        }
        err("unknown CALIBRATE subcommand (try HELP)");
        return;
    }

    err("unknown command (try HELP)");
}

// ---- public API --------------------------------------------------

void init(Config& live) {
    s_live    = &live;
    s_staging = live;   // seed staging from what main.cpp is running
    s_len     = 0;
    s_line[0] = '\0';
    Serial.println("SerialConsole: ready (type HELP)");
}

void tick() {
    while (Serial.available() > 0) {
        int c = Serial.read();
        if (c < 0) break;

        if (c == '\r' || c == '\n') {
            if (s_len > 0) {
                s_line[s_len] = '\0';
                // Local echo of the newline so terminals in raw mode
                // see the break between command and response.
                Serial.println();
                dispatch(s_line);
                s_len = 0;
            }
            continue;
        }

        // Backspace / DEL — rudimentary editing so a human on a
        // terminal can fix typos. The webflasher sends whole lines
        // at a time and never triggers this branch.
        if (c == 0x08 || c == 0x7F) {
            if (s_len > 0) {
                s_len--;
                Serial.print("\b \b");
            }
            continue;
        }

        if (s_len < sizeof(s_line) - 1) {
            s_line[s_len++] = (char)c;
            Serial.write((uint8_t)c);  // local echo
        } else {
            // Overflow: drop the line, tell the user.
            s_len = 0;
            Serial.println();
            err("line too long");
        }
    }
}

Config& staging() { return s_staging; }

}} // namespace rlr::serial_console
