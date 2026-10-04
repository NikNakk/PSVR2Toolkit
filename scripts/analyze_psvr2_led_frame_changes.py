#!/usr/bin/env python3
"""Compare PS VR2 tracking-camera frames immediately before/after native LED events.

Input is a capture directory produced by experiment/native-optical-capture.
For each event with a rel=-1 frame and rel=+1 frame, the script decodes both
stereo BC4 images, writes absolute-difference PGM images, and reports connected
regions whose luma change exceeds a threshold.

This is intentionally a behavioural/camera-I/O analysis. It does not consume
Sony internal optical-tracker data.

Usage:
  py scripts/analyze_psvr2_led_frame_changes.py "%TEMP%\\psvr2-toolkit-optical-12345"
  py scripts/analyze_psvr2_led_frame_changes.py capture-dir --threshold 24 --min-area 2
"""

from __future__ import annotations

import argparse
import csv
from collections import deque
from pathlib import Path

from decode_psvr2_vi11 import (
    BC4_EYE_SIZE,
    HEADER_SIZE,
    decode_eye,
    write_pgm,
)

WIDTH = 1016
HEIGHT = 1016


def load_eye(path: Path, eye: str, gain: float) -> bytearray:
    raw = path.read_bytes()
    payload = raw[HEADER_SIZE : HEADER_SIZE + 2 * BC4_EYE_SIZE]
    if len(payload) != 2 * BC4_EYE_SIZE:
        raise ValueError(f"{path}: short VI11 payload")
    if eye == "left":
        return decode_eye(payload[:BC4_EYE_SIZE], gain)
    return decode_eye(payload[BC4_EYE_SIZE:], gain)


def abs_diff(a: bytes, b: bytes) -> bytearray:
    if len(a) != len(b):
        raise ValueError("image sizes differ")
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
        sum_x = 0
        sum_y = 0
        sum_change = 0
        peak = 0
        min_x = WIDTH
        min_y = HEIGHT
        max_x = 0
        max_y = 0

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

            for nx, ny in ((x - 1, y), (x + 1, y), (x, y - 1), (x, y + 1),
                           (x - 1, y - 1), (x + 1, y - 1), (x - 1, y + 1), (x + 1, y + 1)):
                if nx < 0 or nx >= WIDTH or ny < 0 or ny >= HEIGHT:
                    continue
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
    ap.add_argument("--gain", type=float, default=1.0)
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
        "event_id", "kind", "side", "payload_hex", "eye", "region_rank",
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

            for eye in ("left", "right"):
                pre = load_eye(pre_path, eye, args.gain)
                post = load_eye(post_path, eye, args.gain)
                diff = abs_diff(pre, post)

                prefix = out_dir / f"event-{int(event_id):04d}-{eye}"
                write_pgm(prefix.with_name(prefix.name + "-pre.pgm"), pre)
                write_pgm(prefix.with_name(prefix.name + "-post.pgm"), post)
                write_pgm(prefix.with_name(prefix.name + "-diff.pgm"), diff)

                regs = components(diff, args.threshold, args.min_area)
                for rank, region in enumerate(regs[: args.max_regions], start=1):
                    writer.writerow({
                        "event_id": event_id,
                        "kind": event.get("kind", ""),
                        "side": event.get("side", ""),
                        "payload_hex": event.get("payload_hex", ""),
                        "eye": eye,
                        "region_rank": rank,
                        **region,
                        "pre_file": pre_path.name,
                        "post_file": post_path.name,
                    })

                print(
                    f"event {event_id} {eye}: {len(regs)} regions >= threshold {args.threshold}; "
                    f"wrote pre/post/diff"
                )

    print(f"analysed {completed} events; region summary: {summary_path}")


if __name__ == "__main__":
    main()
