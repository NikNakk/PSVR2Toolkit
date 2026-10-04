#!/usr/bin/env python3
"""Extract native Sony Sense A2/31 LED schedules from a Windows ETW pcapng capture.

No third-party modules are required. The capture produced by Wireshark etwdump
uses LINKTYPE_ETW (290); rather than depending on ETW provider metadata, this
tool scans each Enhanced Packet Block for HIDP output 0xA2 + report 0x31 and
accepts only records whose Sony CRC32 is valid.

Usage:
  python scripts/analyze_sony_sense_etw_pcap.py steamvr-success.pcapng.gz
  python scripts/analyze_sony_sense_etw_pcap.py capture.pcapng --csv sense-a231.csv
"""

from __future__ import annotations

import argparse
import csv
import gzip
import struct
import zlib
from collections import Counter
from datetime import datetime, timezone
from pathlib import Path

PCAPNG_EPB = 0x00000006


def read_capture(path: Path) -> bytes:
    if path.suffix.lower() == ".gz":
        with gzip.open(path, "rb") as f:
            return f.read()
    return path.read_bytes()


def enhanced_packets(blob: bytes):
    off = 0
    while off + 12 <= len(blob):
        block_type, block_len = struct.unpack_from("<II", blob, off)
        if block_len < 12 or off + block_len > len(blob):
            raise ValueError(f"invalid pcapng block at 0x{off:x}: len={block_len}")
        if struct.unpack_from("<I", blob, off + block_len - 4)[0] != block_len:
            raise ValueError(f"pcapng trailing block length mismatch at 0x{off:x}")
        if block_type == PCAPNG_EPB:
            body = memoryview(blob)[off + 8 : off + block_len - 4]
            if len(body) >= 20:
                _iface, ts_hi, ts_lo, cap_len, _packet_len = struct.unpack_from("<IIIII", body, 0)
                packet = bytes(body[20 : 20 + cap_len])
                yield ((ts_hi << 32) | ts_lo), packet
        off += block_len


def reports(blob: bytes):
    for capture_ts_us, packet in enhanced_packets(blob):
        start = 0
        while True:
            pos = packet.find(b"\xa2\x31", start)
            if pos < 0:
                break
            start = pos + 1
            if pos + 79 > len(packet):
                continue
            hidp = packet[pos]
            report = packet[pos + 1 : pos + 79]
            if report[0] != 0x31:
                continue
            expected_crc = int.from_bytes(report[74:78], "little")
            actual_crc = zlib.crc32(bytes([hidp]) + report[:74]) & 0xFFFFFFFF
            if expected_crc != actual_crc:
                continue

            yield {
                "capture_ts_us": capture_ts_us,
                "capture_utc": datetime.fromtimestamp(capture_ts_us / 1_000_000, timezone.utc).isoformat(),
                "mode": report[1],
                "flags": report[2],
                "report_timestamp": int.from_bytes(report[17:21], "little"),
                "phase": report[21],
                "sequence": report[22],
                "period": report[23],
                "cycle_position": int.from_bytes(report[24:28], "little", signed=True),
                "cycle_length": int.from_bytes(report[28:32], "little"),
                "led0": report[32],
                "led1": report[33],
                "led2": report[34],
                "led3": report[35],
            }


def setting_key(r):
    return (
        r["phase"],
        r["sequence"],
        r["period"],
        r["cycle_position"],
        r["cycle_length"],
        r["led0"],
        r["led1"],
        r["led2"],
        r["led3"],
    )


def setting_runs(rows):
    if not rows:
        return []
    result = []
    start = 0
    for i in range(1, len(rows) + 1):
        if i == len(rows) or setting_key(rows[i]) != setting_key(rows[start]):
            first = rows[start]
            last = rows[i - 1]
            result.append(
                {
                    **{k: first[k] for k in (
                        "phase", "sequence", "period", "cycle_position", "cycle_length",
                        "led0", "led1", "led2", "led3"
                    )},
                    "start_utc": first["capture_utc"],
                    "duration_s": (last["capture_ts_us"] - first["capture_ts_us"]) / 1_000_000,
                    "reports": i - start,
                }
            )
            start = i
    return result


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("capture", type=Path)
    ap.add_argument("--csv", type=Path, help="write every valid A2/31 report as CSV")
    ap.add_argument("--runs-csv", type=Path, help="write schedule-setting runs as CSV")
    args = ap.parse_args()

    rows = list(reports(read_capture(args.capture)))
    if not rows:
        raise SystemExit("No CRC-valid A2/31 reports found")

    phases = Counter(r["phase"] for r in rows)
    phase_periods = Counter((r["phase"], r["period"]) for r in rows)
    masks = Counter((r["led0"], r["led1"], r["led2"], r["led3"]) for r in rows if r["phase"] in (2, 3))
    duration = (rows[-1]["capture_ts_us"] - rows[0]["capture_ts_us"]) / 1_000_000

    print(f"valid A2/31 reports: {len(rows)}")
    print(f"capture span: {duration:.6f} s")
    if duration > 0:
        print(f"mean report rate: {(len(rows) - 1) / duration:.3f} Hz")
    print("phase counts:")
    for phase, count in sorted(phases.items()):
        periods = ", ".join(
            f"period {period}: {n}" for (p, period), n in sorted(phase_periods.items()) if p == phase
        )
        print(f"  phase {phase}: {count} ({periods})")
    print("BROAD/BG LED tuples:")
    for mask, count in masks.most_common():
        print(f"  {' '.join(f'{x:02x}' for x in mask)}: {count}")

    runs = setting_runs(rows)
    print(f"schedule setting runs: {len(runs)}")
    for run in runs:
        print(
            f"  {run['start_utc']} phase={run['phase']} seq={run['sequence']} period={run['period']} "
            f"pos={run['cycle_position']} len={run['cycle_length']} "
            f"leds={run['led0']:02x} {run['led1']:02x} {run['led2']:02x} {run['led3']:02x} "
            f"reports={run['reports']} duration={run['duration_s']:.3f}s"
        )

    if args.csv:
        with args.csv.open("w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
            w.writeheader()
            w.writerows(rows)

    if args.runs_csv:
        with args.runs_csv.open("w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=list(runs[0].keys()))
            w.writeheader()
            w.writerows(runs)


if __name__ == "__main__":
    main()
