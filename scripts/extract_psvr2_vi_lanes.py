#!/usr/bin/env python3
"""Inspect and extract observable byte lanes from PS VR2 1,040,640-byte VI packets.

This deliberately avoids assigning proprietary semantics to the payload.
For packets of the size observed from camera mode 0x10, the bytes after the
256-byte VI header fit exactly as:

  508 rows * (254 samples * 8 byte lanes + 16 bytes row padding)

The script writes each of the eight byte lanes as a 254x508 8-bit PGM image.
That makes before/after LED changes spatially inspectable without assuming
that the payload is BC4 or naming the lanes/cameras prematurely.

Usage:
  py scripts/extract_psvr2_vi_lanes.py camera-0001-event-0001-rel-m01-set-11-ts-123.vi
  py scripts/extract_psvr2_vi_lanes.py capture-dir
"""

from __future__ import annotations

import argparse
import struct
from pathlib import Path

HEADER_SIZE = 256
ROWS = 508
SAMPLES_PER_ROW = 254
LANES = 8
ROW_PADDING = 16
ROW_BYTES = SAMPLES_PER_ROW * LANES + ROW_PADDING
PAYLOAD_SIZE = ROWS * ROW_BYTES
PACKET_SIZE = HEADER_SIZE + PAYLOAD_SIZE


def parse_header(raw: bytes) -> dict[str, int]:
    if len(raw) < 28 or raw[:2] != b"VI":
        raise ValueError("not a VI packet")
    (
        version,
        packet_size,
        vts_us,
        sequence_id,
        camera_set,
        image_height,
        active_height,
        image_width,
        active_width,
        unknown2,
    ) = struct.unpack_from("<HIIIHHHHHH", raw, 2)
    return {
        "version": version,
        "packet_size": packet_size,
        "vts_us": vts_us,
        "sequence_id": sequence_id,
        "camera_set": camera_set,
        "image_height": image_height,
        "active_height": active_height,
        "image_width": image_width,
        "active_width": active_width,
        "unknown2": unknown2,
    }


def write_pgm(path: Path, pixels: bytes) -> None:
    with path.open("wb") as f:
        f.write(f"P5\n{SAMPLES_PER_ROW} {ROWS}\n255\n".encode("ascii"))
        f.write(pixels)


def extract(path: Path) -> None:
    raw = path.read_bytes()
    hdr = parse_header(raw)
    print(
        f"{path.name}: size={len(raw)} header.packet_size={hdr['packet_size']} "
        f"set={hdr['camera_set']} seq={hdr['sequence_id']} vts={hdr['vts_us']} "
        f"image={hdr['image_width']}x{hdr['image_height']} "
        f"active={hdr['active_width']}x{hdr['active_height']}"
    )

    if len(raw) != PACKET_SIZE:
        print(f"  skipping lane extraction: expected {PACKET_SIZE} bytes for observed mode-0x10 packing")
        return

    payload = memoryview(raw)[HEADER_SIZE:]
    planes = [bytearray(SAMPLES_PER_ROW * ROWS) for _ in range(LANES)]

    for y in range(ROWS):
        row = payload[y * ROW_BYTES : (y + 1) * ROW_BYTES]
        for x in range(SAMPLES_PER_ROW):
            base = x * LANES
            out = y * SAMPLES_PER_ROW + x
            for lane in range(LANES):
                planes[lane][out] = row[base + lane]

    for lane, pixels in enumerate(planes):
        out = path.with_name(f"{path.stem}-lane{lane}.pgm")
        write_pgm(out, pixels)
    print("  wrote lanes 0..7 as 254x508 PGM")


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("input", type=Path)
    args = ap.parse_args()

    paths = sorted(args.input.glob("*.vi")) if args.input.is_dir() else [args.input]
    if not paths:
        raise SystemExit("No .vi captures found")
    for path in paths:
        extract(path)


if __name__ == "__main__":
    main()
