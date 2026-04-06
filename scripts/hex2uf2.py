#!/usr/bin/env python3
"""
Convert Intel HEX to UF2 for nRF52840 boards with UF2 bootloader
(like Seeed XIAO nRF52840). Called as a PlatformIO post-build action.

UF2 spec: https://github.com/microsoft/uf2
nRF52840 family ID: 0xADA52840
"""

import struct
import sys
import os

UF2_MAGIC_START0 = 0x0A324655  # "UF2\n"
UF2_MAGIC_START1 = 0x9E5D5157
UF2_MAGIC_END    = 0x0AB16F30
UF2_FLAG_FAMILY  = 0x00002000
NRF52840_FAMILY  = 0xADA52840
BLOCK_SIZE       = 256  # payload bytes per UF2 block


def parse_hex(hex_path):
    """Parse Intel HEX into a dict of {address: bytes}."""
    segments = {}
    base_addr = 0
    with open(hex_path, "r") as f:
        for line in f:
            line = line.strip()
            if not line.startswith(":"):
                continue
            data = bytes.fromhex(line[1:])
            byte_count = data[0]
            addr = (data[1] << 8) | data[2]
            rec_type = data[3]
            payload = data[4:4 + byte_count]

            if rec_type == 0x00:  # Data
                full_addr = base_addr + addr
                for i, b in enumerate(payload):
                    segments[full_addr + i] = b
            elif rec_type == 0x02:  # Extended Segment Address
                base_addr = ((payload[0] << 8) | payload[1]) << 4
            elif rec_type == 0x04:  # Extended Linear Address
                base_addr = ((payload[0] << 8) | payload[1]) << 16
            elif rec_type == 0x01:  # EOF
                break
    return segments


def segments_to_contiguous(segments):
    """Convert sparse address map to list of (start_addr, bytes) tuples."""
    if not segments:
        return []
    addrs = sorted(segments.keys())
    result = []
    start = addrs[0]
    current = bytearray([segments[start]])
    for i in range(1, len(addrs)):
        if addrs[i] == addrs[i - 1] + 1:
            current.append(segments[addrs[i]])
        else:
            result.append((start, bytes(current)))
            start = addrs[i]
            current = bytearray([segments[addrs[i]]])
    result.append((start, bytes(current)))
    return result


def convert_to_uf2(hex_path, uf2_path):
    segments = parse_hex(hex_path)
    regions = segments_to_contiguous(segments)

    # Count total blocks
    total_blocks = 0
    for addr, data in regions:
        total_blocks += (len(data) + BLOCK_SIZE - 1) // BLOCK_SIZE

    blocks = []
    block_no = 0
    for addr, data in regions:
        for offset in range(0, len(data), BLOCK_SIZE):
            chunk = data[offset:offset + BLOCK_SIZE]
            # Pad chunk to exactly 256 bytes. The Adafruit UF2 bootloader's
            # is_uf2_block() requires payloadSize == 256; a short final block
            # is silently rejected, preventing the completion handler from
            # firing and leaving the board stuck in bootloader mode.
            if len(chunk) < BLOCK_SIZE:
                chunk = chunk + b"\x00" * (BLOCK_SIZE - len(chunk))
            padding = b"\x00" * (476 - len(chunk))  # pad to 476 bytes
            block = struct.pack("<IIIIIIII",
                                UF2_MAGIC_START0,
                                UF2_MAGIC_START1,
                                UF2_FLAG_FAMILY,
                                addr + offset,
                                BLOCK_SIZE,
                                block_no,
                                total_blocks,
                                NRF52840_FAMILY)
            block += chunk + padding
            block += struct.pack("<I", UF2_MAGIC_END)
            assert len(block) == 512
            blocks.append(block)
            block_no += 1

    with open(uf2_path, "wb") as f:
        for b in blocks:
            f.write(b)

    print(f"hex2uf2: {hex_path} -> {uf2_path} ({len(blocks)} blocks, {len(blocks) * 512} bytes)")


if __name__ == "__main__":
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} input.hex output.uf2")
        sys.exit(1)
    convert_to_uf2(sys.argv[1], sys.argv[2])
