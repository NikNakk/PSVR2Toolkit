#!/usr/bin/env python3
"""Compare observable PS VR2 VI byte-lane images around native LED events.

This uses only the raw USB packet layout:
  256-byte VI header
  508 rows * (254 samples * 8 byte lanes + 16 row-padding bytes)

It deliberately does not assign camera/colour/codec semantics to the eight
lanes. For each event with rel=-1 and rel=+1 frames, it writes absolute
difference PGM images and connected changed regions for every lane.

Usage:
  py scripts/analyze_psvr2_led_frame_changes.py "%TEMP%\\psvr2-toolkit-optical-12345"
  py scripts/analyze_psvr2_led_frame_changes.py capture-dir --threshold 24 --min-area 2
"""

from __future__ import annotations

import argparse
import csv
from collections import deque
from pathlib import Path

HEADER_SIZE = 256
HEIGHT = 508
WIDTH = 254
LANES = 8
ROW_PADDING = 16
ROW_BYTES = WIDTH * LANES + ROW_PADDING
PACKET_SIZE = HEADER_SIZE + HEIGHT * ROW_BYTES


def write_pgm(path: Path, pixels: bytes) -> None:
    with path.open("wb") as f:
        f.write(f"P5\n{WIDTH} {HEIGHT}\n255\n".encode("ascii"))
        f.write(pixels)


def load_lanes(path: Path) -> list[bytearray]:
    raw = path.read_bytes()
    if len(raw) != PACKET_SIZE or raw[:2] != b"VI":
        raise ValueError(f"{path}: expected {PACKET_SIZE}-byte VI packet, got {len(raw)}")

    payload = memoryview(raw)[HEADER_SIZE:]
    planes = [bytearray(WIDTH * HEIGHT) for _ in range(LANES)]
    for y in range(HEIGHT):
        row = payload[y * ROW_BYTES : (y + 1) * ROW_BYTES]
        for x in range(WIDTH):
            base = x * LANES
            out = y * WIDTH + x
            for lane in range(LANES):
                planes[lane][out] = row[base + lane]
    return planes


def abs_diff(a: bytes, b: bytes) -> bytearray:
    return bytearray(abs(x - y) for x, y in zip(a, b))


def components(diff: bytes, threshold: int, min_area: int):
    active = bytearray(1 if v >= threshold else 0 for v in diff)
    regions = []

    for start, on in enumerate(active):
        if not on:
            continue
        active[start] = 0
        q = deque([start])
        area = 0
        sum_x = sum_y = sum_change = 0
        peak = 0
        min_x = WIDTH
        min_y = HEIGHT
        max_x = max_y = 0

        while q:
            idx = q.popleft()
            y, x = divmod(idx, WIDTH)
            value = diff[idx]

            area += 1
            sum_x += x
            sum_y += y
            sum_change += value
            peak = max(peak, value)
            min_x = min(min_x, x)
            max_x = max(max_x, x)
            min_y = min(min_y, y)
            max_y = max(max_y, y)

            for nx, ny in (
                (x - 1, y), (x + 1, y), (x, y - 1), (x, y + 1),
                (x - 1, y - 1), (x + 1, y - 1), (x - 1, y + 1), (x + 1, y + 1),
            ):
                if 0 <= nx < WIDTH and 0 <= ny < HEIGHT:
                    ni = ny * WIDTH + nx
                    if active[ni]:
                        active[ni] = 0
                        q.append(ni)

        if area >= min_area:
            regions.append({
                "area": area,
                "centroid_x": sum_x / area,
                "centroid_y": sum_y / area,
                "min_x": min_x,
                "min_y": min_y,
                "max_x": max_x,
                "max_y": max_y,
                "peak": peak,
                "mean_change": sum_change / area,
            })

    regions.sort(key=lambda r: (r["peak"], r["area"], r["mean_change"]), reverse=True)
    return regions


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("capture_dir", type=Path)
    ap.add_argument("--threshold", type=int, default=20)
    ap.add_argument("--min-area", type=int, default=2)
    ap.add_argument("--max-regions", type=int, default=32)
    args = ap.parse_args()

    frames_csv = args.capture_dir / "camera_frames.csv"
    events_csv = args.capture_dir / "events.csv"
    if not frames_csv.exists() or not events_csv.exists():
        raise SystemExit("capture directory must contain camera_frames.csv and events.csv")

    with frames_csv.open(newline="") as f:
        frame_rows = list(csv.DictReader(f))
    with events_csv.open(newline="") as f:
        event_rows = {row["event_id"]: row for row in csv.DictReader(f)}

    by_event: dict[str, dict[int, dict[str, str]]] = {}
    for row in frame_rows:
        by_event.setdefault(row["event_id"], {})[int(row["relative_frame"])] = row

    out_dir = args.capture_dir / "led-frame-diffs"
    out_dir.mkdir(exist_ok=True)
    summary_path = out_dir / "regions.csv"

    fields = [
        "event_id", "kind", "side", "detail", "lane", "region_rank",
        "area", "centroid_x", "centroid_y", "min_x", "min_y", "max_x", "max_y",
        "peak", "mean_change", "pre_file", "post_file",
    ]

    completed = 0
    with summary_path.open("w", newline="") as sf:
        writer = csv.DictWriter(sf, fieldnames=fields)
        writer.writeheader()

        for event_id, rels in sorted(by_event.items(), key=lambda kv: int(kv[0])):
            if -1 not in rels or 1 not in rels:
                continue

            pre_path = args.capture_dir / rels[-1]["filename"]
            post_path = args.capture_dir / rels[1]["filename"]
            event = event_rows.get(event_id, {})
            completed += 1

            pre_lanes = load_lanes(pre_path)
            post_lanes = load_lanes(post_path)
            for lane, (pre, post) in enumerate(zip(pre_lanes, post_lanes)):
                diff = abs_diff(pre, post)
                prefix = out_dir / f"event-{int(event_id):04d}-lane{lane}"
                write_pgm(prefix.with_name(prefix.name + "-pre.pgm"), pre)
                write_pgm(prefix.with_name(prefix.name + "-post.pgm"), post)
                write_pgm(prefix.with_name(prefix.name + "-diff.pgm"), diff)

                regs = components(diff, args.threshold, args.min_area)
                for rank, region in enumerate(regs[: args.max_regions], start=1):
                    writer.writerow({
                        "event_id": event_id,
                        "kind": event.get("kind", ""),
                        "side": event.get("side", ""),
                        "detail": event.get("detail", ""),
                        "lane": lane,
                        "region_rank": rank,
                        **region,
                        "pre_file": pre_path.name,
                        "post_file": post_path.name,
                    })

                print(
                    f"event {event_id} lane {lane}: {len(regs)} regions >= threshold {args.threshold}"
                )

    print(f"analysed {completed} events; region summary: {summary_path}")


if __name__ == "__main__":
    main()
