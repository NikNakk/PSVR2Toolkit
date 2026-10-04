#!/usr/bin/env python3
"""Decode PS VR2 type-11 tracking camera captures written by PSVR2Toolkit.

Input .vi11 files contain the 256-byte Sony VI header followed by two BC4
1024x1016 images. The useful image width is 1016 pixels; the final 8 columns
are texture padding. This script writes portable 8-bit PGM files using only
the Python standard library.

Usage:
  python scripts/decode_psvr2_vi11.py camera-0001-event-0001-ts-123.vi11
  python scripts/decode_psvr2_vi11.py capture-dir --gain 2.0
"""

from __future__ import annotations

import argparse
from pathlib import Path

HEADER_SIZE = 256
SOURCE_WIDTH = 1024
OUTPUT_WIDTH = 1016
HEIGHT = 1016
BC4_EYE_SIZE = SOURCE_WIDTH * HEIGHT // 2
EXPECTED_SIZE = HEADER_SIZE + 2 * BC4_EYE_SIZE


def bc4_palette(a0: int, a1: int) -> list[int]:
    p = [a0, a1]
    if a0 > a1:
        p.extend([
            (6 * a0 + 1 * a1) // 7,
            (5 * a0 + 2 * a1) // 7,
            (4 * a0 + 3 * a1) // 7,
            (3 * a0 + 4 * a1) // 7,
            (2 * a0 + 5 * a1) // 7,
            (1 * a0 + 6 * a1) // 7,
        ])
    else:
        p.extend([
            (4 * a0 + 1 * a1) // 5,
            (3 * a0 + 2 * a1) // 5,
            (2 * a0 + 3 * a1) // 5,
            (1 * a0 + 4 * a1) // 5,
            0,
            255,
        ])
    return p


def decode_eye(data: bytes, gain: float) -> bytearray:
    if len(data) != BC4_EYE_SIZE:
        raise ValueError(f"expected {BC4_EYE_SIZE} BC4 bytes, got {len(data)}")

    out = bytearray(OUTPUT_WIDTH * HEIGHT)
    blocks_x = SOURCE_WIDTH // 4
    blocks_y = HEIGHT // 4

    for by in range(blocks_y):
        for bx in range(blocks_x):
            off = (by * blocks_x + bx) * 8
            block = data[off : off + 8]
            palette = bc4_palette(block[0], block[1])
            indices = int.from_bytes(block[2:8], "little")

            x0 = bx * 4
            y0 = by * 4
            for ly in range(4):
                y = y0 + ly
                if y >= HEIGHT:
                    continue
                row = y * OUTPUT_WIDTH
                for lx in range(4):
                    x = x0 + lx
                    if x >= OUTPUT_WIDTH:
                        continue
                    idx = (indices >> (3 * (ly * 4 + lx))) & 0x7
                    value = palette[idx]
                    value = max(0, min(255, round(value * gain)))
                    out[row + x] = value
    return out


def write_pgm(path: Path, pixels: bytes) -> None:
    with path.open("wb") as f:
        f.write(f"P5\n{OUTPUT_WIDTH} {HEIGHT}\n255\n".encode("ascii"))
        f.write(pixels)


def decode_file(path: Path, gain: float) -> None:
    raw = path.read_bytes()
    if len(raw) < EXPECTED_SIZE:
        raise ValueError(f"{path}: expected at least {EXPECTED_SIZE} bytes, got {len(raw)}")

    payload = raw[HEADER_SIZE : HEADER_SIZE + 2 * BC4_EYE_SIZE]
    left = decode_eye(payload[:BC4_EYE_SIZE], gain)
    right = decode_eye(payload[BC4_EYE_SIZE:], gain)

    left_path = path.with_name(path.stem + "-left.pgm")
    right_path = path.with_name(path.stem + "-right.pgm")
    write_pgm(left_path, left)
    write_pgm(right_path, right)
    print(f"{path.name}: wrote {left_path.name}, {right_path.name}")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("input", type=Path, help=".vi11 file or capture directory")
    parser.add_argument("--gain", type=float, default=1.0, help="multiply decoded luma (default 1.0)")
    args = parser.parse_args()

    paths = sorted(args.input.glob("*.vi11")) if args.input.is_dir() else [args.input]
    if not paths:
        raise SystemExit("No .vi11 captures found")
    for path in paths:
        decode_file(path, args.gain)


if __name__ == "__main__":
    main()
