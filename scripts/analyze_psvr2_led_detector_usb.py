#!/usr/bin/env python3
"""Black-box analysis of captured PSVR2 IF8 / endpoint 0x89 LED-detector USB packets.

The input is usb-if8-led-detector.bin produced by experiment/native-optical-capture.
The script knows only the local ULD8 wrapper. It does not assume a Sony payload
structure.

It reports packet cadence/size, byte offsets that change most often, and likely
little-endian u32 counters/timestamps in the first part of the payload by
comparing their modulo-2^32 deltas with host-QPC time deltas.

Usage:
  py scripts/analyze_psvr2_led_detector_usb.py usb-if8-led-detector.bin
  py scripts/analyze_psvr2_led_detector_usb.py usb-if8-led-detector.bin --byte-stats byte-stats.csv
"""

from __future__ import annotations

import argparse
import csv
import statistics
import struct
from dataclasses import dataclass
from pathlib import Path

MAGIC = 0x38444C55  # 'ULD8' little endian
HEADER = struct.Struct("<I H B B Q I")


@dataclass
class Record:
    host_us: int
    interface: int
    pipe: int
    payload: bytes


def iter_records(path: Path):
    """Stream validated local wrappers, retaining only one USB payload."""
    with path.open("rb") as f:
        while True:
            offset = f.tell()
            header = f.read(HEADER.size)
            if not header:
                return
            if len(header) != HEADER.size:
                raise ValueError(f"truncated ULD8 header at {offset:#x}")
            magic, version, interface, pipe, host_us, size = HEADER.unpack(header)
            if magic != MAGIC or version != 1:
                raise ValueError(f"invalid ULD8 wrapper at {offset:#x}")
            if size > 16 * 1024 * 1024:
                raise ValueError(f"implausible payload size {size} at {offset:#x}")
            payload = f.read(size)
            if len(payload) != size:
                raise ValueError(f"truncated ULD8 payload at {offset:#x}")
            yield Record(host_us, interface, pipe, payload)


def load_records(path: Path) -> list[Record]:
    """Compatibility helper; whole-capture callers should use iter_records."""
    return list(iter_records(path))


def median(values):
    return statistics.median(values) if values else float("nan")


def byte_stats(records: list[Record]):
    if not records:
        return []
    size = min(len(r.payload) for r in records)
    n = len(records)
    out = []
    for off in range(size):
        vals = [r.payload[off] for r in records]
        changes = sum(a != b for a, b in zip(vals, vals[1:]))
        out.append({
            "offset": off,
            "unique": len(set(vals)),
            "changes": changes,
            "change_fraction": changes / max(n - 1, 1),
            "min": min(vals),
            "max": max(vals),
        })
    return out


def timestamp_candidates(records: list[Record], scan_bytes: int = 512):
    if len(records) < 4:
        return []

    count = min(len(records), 1000)
    recs = records[:count]
    payload_size = min(len(r.payload) for r in recs)
    limit = min(scan_bytes, payload_size - 3)

    host_deltas = [b.host_us - a.host_us for a, b in zip(recs, recs[1:])]
    candidates = []

    # Search every byte offset: packed protocol fields need not be 4-byte aligned.
    for off in range(max(limit, 0)):
        vals = [int.from_bytes(r.payload[off:off+4], "little") for r in recs]
        deltas = [((b - a) & 0xFFFFFFFF) for a, b in zip(vals, vals[1:])]

        nonzero = sum(d != 0 for d in deltas)
        if nonzero < len(deltas) * 0.8:
            continue

        ratios = []
        relative_errors = []
        for hd, dd in zip(host_deltas, deltas):
            if hd <= 0 or dd == 0 or dd > 10_000_000:
                continue
            ratios.append(dd / hd)

        if len(ratios) < len(deltas) * 0.7:
            continue

        scale = median(ratios)
        if not (0.0001 <= scale <= 10000):
            continue

        for hd, dd in zip(host_deltas, deltas):
            expected = hd * scale
            if hd > 0 and dd > 0 and expected > 0:
                relative_errors.append(abs(dd - expected) / expected)

        score = median(relative_errors)
        candidates.append({
            "offset": off,
            "scale_units_per_host_us": scale,
            "median_relative_error": score,
            "first": vals[0],
            "last": vals[-1],
        })

    candidates.sort(key=lambda x: x["median_relative_error"])
    return candidates


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("capture", type=Path)
    ap.add_argument("--byte-stats", type=Path)
    ap.add_argument("--scan-header-bytes", type=int, default=512)
    args = ap.parse_args()

    records = []
    sizes = {}
    count = 0
    first_us = last_us = None
    all_dts = []
    interfaces, pipes = set(), set()
    changes = minima = maxima = seen = previous = None
    for r in iter_records(args.capture):
        count += 1
        sizes[len(r.payload)] = sizes.get(len(r.payload), 0) + 1
        interfaces.add(r.interface)
        pipes.add(r.pipe)
        if first_us is None:
            first_us = r.host_us
        if last_us is not None:
            all_dts.append(r.host_us - last_us)
        last_us = r.host_us
        if len(records) < 1000:
            records.append(Record(r.host_us, r.interface, r.pipe, r.payload[:args.scan_header_bytes + 4]))
        if args.byte_stats:
            if previous is None:
                changes = [0] * len(r.payload)
                minima, maxima = list(r.payload), list(r.payload)
                seen = [0] * len(r.payload)
            if len(r.payload) != len(changes):
                raise ValueError("full byte statistics require constant payload length")
            for i, v in enumerate(r.payload):
                changes[i] += previous is not None and previous[i] != v
                minima[i] = min(minima[i], v)
                maxima[i] = max(maxima[i], v)
                seen[i] |= 1 << v
            previous = r.payload
    if not count:
        raise SystemExit("No ULD8 records found")
    dts = all_dts
    span_s = (last_us - first_us) / 1_000_000

    print(f"records: {count}")
    print(f"interfaces: {sorted(interfaces)}")
    print(f"pipes: {[hex(x) for x in sorted(pipes)]}")
    print(f"payload sizes: {sizes}")
    print(f"span: {span_s:.6f} s")
    if span_s > 0:
        print(f"mean rate: {(count-1)/span_s:.3f} Hz")
    if dts:
        print(f"median interval: {median(dts):.3f} us")

    stats = ([{"offset": i, "unique": seen[i].bit_count(), "changes": changes[i],
               "change_fraction": changes[i] / max(count - 1, 1), "min": minima[i], "max": maxima[i]}
              for i in range(len(changes))] if args.byte_stats else byte_stats(records))
    hottest = sorted(stats, key=lambda x: (x["change_fraction"], x["unique"]), reverse=True)[:32]
    print("\nMost frequently changing byte offsets (whole stream with --byte-stats; otherwise first 1000 header samples):")
    for s in hottest:
        print(
            f"  {s['offset']:5d} / 0x{s['offset']:04x}: "
            f"change={s['change_fraction']:.3f} unique={s['unique']} "
            f"range={s['min']:02x}-{s['max']:02x}"
        )

    print("\nLikely u32 counter/timestamp fields in packet header:")
    candidates = timestamp_candidates(records, args.scan_header_bytes)
    for c in candidates[:24]:
        print(
            f"  offset {c['offset']:4d} / 0x{c['offset']:04x}: "
            f"scale={c['scale_units_per_host_us']:.9g} units/us "
            f"median_error={c['median_relative_error']:.4f} "
            f"first={c['first']} last={c['last']}"
        )

    if args.byte_stats:
        with args.byte_stats.open("w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=list(stats[0].keys()))
            w.writeheader()
            w.writerows(stats)
        print(f"\nwrote {args.byte_stats}")


if __name__ == "__main__":
    main()
