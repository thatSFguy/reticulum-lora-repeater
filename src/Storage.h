#pragma once
// src/Storage.h — owns the microStore FileSystem instance that backs
// all persistent state: runtime config (/config.bin), Reticulum
// transport identity, and the path table.
//
// Split out of Transport so it can be mounted EARLY in setup() —
// before Config::load_or_defaults() runs, which needs the filesystem
// registered to read /config.bin. Transport used to own this during
// Phase 2 but the load-order dependency pushed it up.
//
// One job: mount the InternalFS partition and hand it to
// RNS::Utilities::OS via register_filesystem(). After init() returns
// successfully every subsystem that touches files via the RNS OS
// shim (Identity, path_store, Config) resolves to this instance.

namespace rlr { namespace storage {

// Initialise the internal flash filesystem and register it with the
// microReticulum OS shim. Must be called before any code that touches
// the filesystem — Config::load, Transport::init, Reticulum::start,
// Identity persistence. Returns true on success.
bool init();

}} // namespace rlr::storage
