#pragma once
// =====================================================================
//  src/Msgpack.h — minimal msgpack writer for LXMF / Sideband telemetry.
//
//  Hand-rolled, no Arduino dependency. The firmware previously hand-wrote
//  a single fixed msgpack blob inline in LxmfPresence.cpp; spec-compliant
//  LXMF telemetry (FIELD_TELEMETRY) needs a real encoder that can emit
//  maps, nested values, floats, and binary blobs.
//
//  Encoding follows the umsgpack "canonical" rules the upstream Python
//  stack signs against (reticulum-specifications SPEC.md §5.6.1):
//    - integers: smallest signed/unsigned envelope that fits
//    - floats:   ALWAYS float64 (0xcb), never float32 — even for
//                integer-valued doubles (the LXMF message timestamp)
//    - text:     str family (Python str)
//    - bytes:    bin family (Python bytes)
//  Getting these widths right is what makes a receiver's raw-bytes
//  signature check (SPEC §5.6 path 1) succeed instead of relying on the
//  defensive decode-then-re-encode fallback.
//
//  All multi-byte values are big-endian per the msgpack spec.
// =====================================================================

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <vector>
#include <string>

namespace rlr { namespace msgpack {

class Writer {
public:
    void reserve(size_t n) { _buf.reserve(n); }

    // ---- containers ----
    // n = number of elements (array) or key/value PAIRS (map).
    void array_header(size_t n);
    void map_header(size_t n);

    // ---- scalars ----
    void nil();
    void boolean(bool v);

    // Unsigned integer, smallest envelope that fits (positive fixint,
    // uint8/16/32/64).
    void uint(uint64_t v);
    // Signed integer, smallest envelope that fits. Non-negative values
    // route through uint() so they match umsgpack's "prefer positive
    // fixint / uintN" choice.
    void integer(int64_t v);

    // IEEE-754 double, always emitted as float64 (SPEC §5.6.1).
    void float64(double v);

    // Text (msgpack str family).
    void str(const char* s, size_t len);
    void str(const std::string& s) { str(s.data(), s.size()); }
    void str(const char* s) { str(s, strlen(s)); }

    // Binary blob (msgpack bin family).
    void bin(const uint8_t* data, size_t len);

    // Splice already-encoded msgpack bytes verbatim (e.g. a pre-built
    // value such as the LXMF `fields` map). Caller guarantees validity.
    void append(const uint8_t* data, size_t len);

    // ---- output ----
    const std::vector<uint8_t>& bytes() const { return _buf; }
    const uint8_t* data() const { return _buf.data(); }
    size_t size() const { return _buf.size(); }

private:
    std::vector<uint8_t> _buf;

    void put(uint8_t b)            { _buf.push_back(b); }
    void be16(uint16_t v);
    void be32(uint32_t v);
    void be64(uint64_t v);
};

} } // namespace rlr::msgpack
