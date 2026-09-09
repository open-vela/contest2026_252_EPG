#!/usr/bin/env python3
"""Encode a BK7258 image into the CRC-checked layout the flash controller expects.

The BK7258 flash controller verifies a CRC while fetching instructions over
XIP.  It stores two CRC bytes after every 32 bytes of payload, so 32 bytes of
"virtual" (CPU-visible) data occupy 34 bytes of physical flash.  A raw
``nuttx.bin`` therefore has to be re-encoded before it is programmed.

The CRC is a 16-bit MSB-first CRC with polynomial 0x8005 and an initial value
of 0xFFFF, stored big-endian.  That was determined by brute-forcing the
parameters against the CRC-encoded binaries shipped in the Beken SDK
(bk_idk release/v2.0.1, components/secure_calibration/package_for_calibration)
and verified to reproduce every one of the 3868 blocks in those files exactly.
``--verify`` re-runs that check against any known-good encoded image.

Usage::

    ./bk_crc_pack.py nuttx.bin nuttx_crc.bin
    ./bk_crc_pack.py --verify some_known_crc.bin
"""

import argparse
import sys

BLOCK_DATA = 32
BLOCK_CRC = 2
BLOCK_TOTAL = BLOCK_DATA + BLOCK_CRC

CRC_POLY = 0x8005
CRC_INIT = 0xFFFF

# Pad byte for the final short block.  Erased flash reads as 0xFF, so padding
# with 0xFF keeps the encoded image consistent with an erased device.
PAD_BYTE = 0xFF


def crc16(data: bytes, crc: int = CRC_INIT) -> int:
    """MSB-first CRC-16, polynomial 0x8005, initial value 0xFFFF."""
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ CRC_POLY) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc


def encode(payload: bytes) -> bytes:
    """Interleave CRCs into payload, padding the last block if needed."""
    if len(payload) % BLOCK_DATA:
        payload += bytes([PAD_BYTE]) * (BLOCK_DATA - len(payload) % BLOCK_DATA)

    out = bytearray()
    for off in range(0, len(payload), BLOCK_DATA):
        block = payload[off:off + BLOCK_DATA]
        out += block
        out += crc16(block).to_bytes(2, "big")
    return bytes(out)


def verify(encoded: bytes) -> int:
    """Return the number of blocks whose stored CRC does not match."""
    if len(encoded) % BLOCK_TOTAL:
        raise SystemExit(
            f"not a CRC-encoded image: length {len(encoded)} is not a "
            f"multiple of {BLOCK_TOTAL}"
        )

    bad = 0
    for off in range(0, len(encoded), BLOCK_TOTAL):
        block = encoded[off:off + BLOCK_DATA]
        stored = encoded[off + BLOCK_DATA:off + BLOCK_TOTAL]
        if crc16(block).to_bytes(2, "big") != stored:
            bad += 1
    return bad


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("infile", help="raw image, or encoded image with --verify")
    parser.add_argument("outfile", nargs="?", help="encoded output image")
    parser.add_argument(
        "--verify",
        action="store_true",
        help="check the CRCs of an already-encoded image instead of encoding",
    )
    args = parser.parse_args()

    with open(args.infile, "rb") as handle:
        data = handle.read()

    if args.verify:
        bad = verify(data)
        blocks = len(data) // BLOCK_TOTAL
        print(f"{args.infile}: {blocks} blocks, {bad} CRC mismatches")
        return 1 if bad else 0

    if not args.outfile:
        parser.error("outfile is required unless --verify is given")

    # Tail guard: the ICache prefetches sequentially past the last real
    # instruction, and a fetch that lands in flash the packer never wrote
    # reads a CRC-invalid block and hangs the bus (boot lived or died on
    # a 536-byte size difference before this).  4 KB of valid padding
    # puts the cliff out of prefetch reach forever.
    #
    # This belongs to encoding only.  It used to be applied to whatever was
    # read, which meant --verify padded an already-encoded image and then
    # rejected it for not being a multiple of the block size -- the self
    # check in the board README could never pass.

    data = data + b"\xff" * 4096

    encoded = encode(data)
    with open(args.outfile, "wb") as handle:
        handle.write(encoded)

    print(
        f"{args.infile}: {len(data)} bytes -> {args.outfile}: {len(encoded)} "
        f"bytes ({len(encoded) // BLOCK_TOTAL} blocks)"
    )

    # Re-read what we wrote so a corrupted write cannot go unnoticed.
    bad = verify(encoded)
    if bad:
        print(f"ERROR: {bad} blocks failed verification", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
