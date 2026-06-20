// src/Msgpack.cpp — see Msgpack.h. Emit-only msgpack encoder tuned for
// LXMF payloads and the Sideband Telemeter dict.

#include "Msgpack.h"

namespace rlr { namespace msgpack {

void Writer::be16(uint16_t v) {
    put((uint8_t)(v >> 8));
    put((uint8_t)(v));
}

void Writer::be32(uint32_t v) {
    put((uint8_t)(v >> 24));
    put((uint8_t)(v >> 16));
    put((uint8_t)(v >> 8));
    put((uint8_t)(v));
}

void Writer::be64(uint64_t v) {
    for (int s = 56; s >= 0; s -= 8) put((uint8_t)(v >> s));
}

void Writer::array_header(size_t n) {
    if (n < 16) {
        put((uint8_t)(0x90 | (n & 0x0f)));        // fixarray
    } else if (n < 65536) {
        put(0xdc); be16((uint16_t)n);             // array16
    } else {
        put(0xdd); be32((uint32_t)n);             // array32
    }
}

void Writer::map_header(size_t n) {
    if (n < 16) {
        put((uint8_t)(0x80 | (n & 0x0f)));        // fixmap
    } else if (n < 65536) {
        put(0xde); be16((uint16_t)n);             // map16
    } else {
        put(0xdf); be32((uint32_t)n);             // map32
    }
}

void Writer::nil()           { put(0xc0); }
void Writer::boolean(bool v) { put(v ? 0xc3 : 0xc2); }

void Writer::uint(uint64_t v) {
    if (v < 0x80) {
        put((uint8_t)v);                          // positive fixint
    } else if (v <= 0xff) {
        put(0xcc); put((uint8_t)v);               // uint8
    } else if (v <= 0xffff) {
        put(0xcd); be16((uint16_t)v);             // uint16
    } else if (v <= 0xffffffffULL) {
        put(0xce); be32((uint32_t)v);             // uint32
    } else {
        put(0xcf); be64(v);                       // uint64
    }
}

void Writer::integer(int64_t v) {
    if (v >= 0) { uint((uint64_t)v); return; }
    if (v >= -32) {
        put((uint8_t)(0xe0 | (v & 0x1f)));        // negative fixint
    } else if (v >= -128) {
        put(0xd0); put((uint8_t)(int8_t)v);       // int8
    } else if (v >= -32768) {
        put(0xd1); be16((uint16_t)(int16_t)v);    // int16
    } else if (v >= -2147483648LL) {
        put(0xd2); be32((uint32_t)(int32_t)v);    // int32
    } else {
        put(0xd3); be64((uint64_t)v);             // int64
    }
}

void Writer::float64(double v) {
    uint64_t bits;
    memcpy(&bits, &v, sizeof(bits));              // IEEE-754, host order
    put(0xcb);
    be64(bits);                                   // big-endian on the wire
}

void Writer::str(const char* s, size_t len) {
    if (len < 32) {
        put((uint8_t)(0xa0 | (len & 0x1f)));      // fixstr
    } else if (len < 256) {
        put(0xd9); put((uint8_t)len);             // str8
    } else if (len < 65536) {
        put(0xda); be16((uint16_t)len);           // str16
    } else {
        put(0xdb); be32((uint32_t)len);           // str32
    }
    if (len) _buf.insert(_buf.end(), s, s + len);
}

void Writer::bin(const uint8_t* data, size_t len) {
    if (len < 256) {
        put(0xc4); put((uint8_t)len);             // bin8
    } else if (len < 65536) {
        put(0xc5); be16((uint16_t)len);           // bin16
    } else {
        put(0xc6); be32((uint32_t)len);           // bin32
    }
    if (len) _buf.insert(_buf.end(), data, data + len);
}

void Writer::append(const uint8_t* data, size_t len) {
    if (len) _buf.insert(_buf.end(), data, data + len);
}

} } // namespace rlr::msgpack
