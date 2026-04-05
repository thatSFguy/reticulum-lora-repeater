// src/SerialConsole.cpp — stub. Phase 4 implements the line reader
// + command dispatch. The protocol is intentionally trivial so the
// webflasher's JS client can talk to it with a few dozen lines of
// Web Serial code.
#include "SerialConsole.h"

namespace rlr { namespace serial_console {

static Config s_staging{};

void init() {
    config::defaults(s_staging);
}

void tick() {
    // TODO Phase 4:
    //   - Read Serial into a line buffer, dispatch on newline
    //   - Commands: PING, VERSION, STATUS, HELP, REBOOT,
    //               CONFIG GET, CONFIG SET <key> <val>, CONFIG RESET, CONFIG COMMIT
    //   - Every response terminates with "OK\n" or "ERR: <reason>\n"
    //   - CONFIG COMMIT calls config::save(s_staging) then NVIC_SystemReset()
}

Config& staging() { return s_staging; }

}} // namespace rlr::serial_console
