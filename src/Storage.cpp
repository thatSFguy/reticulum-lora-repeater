// src/Storage.cpp — microStore filesystem mount and RNS::OS registration.
//
// Extracted from Transport.cpp in Phase 3 so it can run EARLY in
// setup() — before Config::load_or_defaults() needs to read /config.bin.
// Previously Transport::init() owned this but the load-order
// dependency between Config and Transport pushed it out.

#include "Storage.h"
#include <Arduino.h>

#include <Utilities/OS.h>

// PlatformIO LDF-visibility include — the transitive include inside
// microStore's adapter header isn't scanned aggressively enough by
// default, so we pull in the Adafruit framework-bundled header at
// top-level here the same way the sibling project does from its
// Utilities.h. Without this the linker can't find InternalFS.
#include <InternalFileSystem.h>
#include <microStore/Adapters/InternalFSFileSystem.h>

namespace rlr { namespace storage {

// File-scope microStore::FileSystem wrapping an InternalFS impl.
// Lives at file scope so its shared_ptr stays alive for the lifetime
// of the program; declaring it inside init() would leave a dead
// pointer after init() returns. Previously lived in Transport.cpp.
static microStore::FileSystem s_filesystem{microStore::Adapters::InternalFSFileSystem()};

bool init() {
    // Two-step dance that every microReticulum example uses:
    //
    //   1. s_filesystem.init() — calls InternalFS.begin() under the
    //      hood to actually mount the littlefs partition. The
    //      InternalFSFileSystem adapter constructor only stores
    //      configuration; nothing is mounted until init() runs.
    //
    //   2. RNS::Utilities::OS::register_filesystem(fs) — hands the
    //      mounted FileSystem to the microReticulum library so
    //      Identity / path_store / Config all resolve file ops
    //      through this backend.
    //
    // If step 1 is skipped every file op throws. If step 2 is skipped
    // RNS throws std::runtime_error("FileSystem has not been
    // registered") the moment reticulum.start() runs.
    Serial.println("Storage: initializing filesystem...");
    s_filesystem.init();
    Serial.println("Storage: registering filesystem with RNS::OS...");
    RNS::Utilities::OS::register_filesystem(s_filesystem);
    return true;
}

}} // namespace rlr::storage
