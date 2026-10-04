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


def stream_enhanced_packets(path: Path, audit=None):
    """Validate every block and honour IDB timestamp resolution/offset (little endian)."""
    audit = audit if audit is not None else Counter()
    opener = gzip.open if path.suffix.lower() == ".gz" else open
    interfaces = []
    with opener(path, "rb") as f:
        while True:
            off = f.tell()
            h = f.read(8)
            if not h:
                return
            if len(h) != 8:
                raise ValueError(f"truncated pcapng header at {off}")
            typ, length = struct.unpack("<II", h)
            if length < 12 or length % 4 or length > 64 * 1024 * 1024:
                raise ValueError(f"invalid pcapng block length at {off}")
            body = f.read(length - 8)
            if len(body) != length - 8 or struct.unpack_from("<I", body, len(body)-4)[0] != length:
                raise ValueError(f"truncated/mismatched pcapng block at {off}")
            audit["blocks"] += 1
            if typ == 0x0a0d0d0a:
                if body[:4] != b"\x4d\x3c\x2b\x1a":
                    raise ValueError("only little-endian pcapng sections supported")
                interfaces = []
            elif typ == 1:
                link, _, snap = struct.unpack_from("<HHI", body)
                resolution, offset = 1e-6, 0
                pos = 8
                while pos + 4 <= len(body)-4:
                    code, size = struct.unpack_from("<HH", body, pos)
                    pos += 4
                    value = body[pos:pos+size]
                    if code == 9 and size == 1:
                        resolution = (2 ** -(value[0] & 127)) if value[0] & 128 else 10 ** -value[0]
                    if code == 14 and size == 8:
                        offset = struct.unpack("<q", value)[0]
                    pos += (size + 3) & ~3
                    if code == 0:
                        break
                interfaces.append((link, resolution, offset))
                audit[f"linktype_{link}"] += 1
            elif typ == PCAPNG_EPB:
                iface, hi, lo, cap, original = struct.unpack_from("<IIIII", body)
                if cap > len(body)-24 or iface >= len(interfaces):
                    raise ValueError(f"invalid EPB at {off}")
                audit["enhanced_packets"] += 1
                audit["truncated_packets"] += cap < original
                _, resolution, offset = interfaces[iface]
                ts = round((((hi << 32) | lo) * resolution + offset) * 1e6)
                yield ts, body[20:20+cap]


def hid_reports(source: bytes | Path, audit=None):
    """CRC-validated HID reports with independently checked adjacent ACL framing.

    ETW payloads in these captures include an ACL header followed by an L2CAP
    header immediately before the report. Expose a handle only when both length
    fields validate; a signature scan alone does not establish device identity.
    """
    packets = stream_enhanced_packets(source, audit) if isinstance(source, Path) else enhanced_packets(source)
    for capture_ts_us, packet in packets:
        for typ in (0xa1, 0xa2):
            start = 0
            while True:
                pos = packet.find(bytes([typ, 0x31]), start)
                if pos < 0:
                    break
                start = pos + 1
                if pos + 79 > len(packet):
                    continue
                report = packet[pos+1:pos+79]
                if zlib.crc32(bytes([typ]) + report[:74]) & 0xffffffff != int.from_bytes(report[74:78], 'little'):
                    continue
                handle = cid = None
                if pos >= 8:
                    acl, acl_length, l2_length, channel = struct.unpack_from('<HHHH', packet, pos-8)
                    if acl_length == 83 and l2_length == 79:
                        handle, cid = acl & 0xfff, channel
                yield capture_ts_us, typ, handle, cid, report


def reports(blob: bytes | Path):
    for capture_ts_us, typ, handle, cid, report in hid_reports(blob):
        if typ != 0xa2:
            continue
        yield {
            "capture_ts_us": capture_ts_us,
            "capture_utc": datetime.fromtimestamp(capture_ts_us / 1_000_000, timezone.utc).isoformat(),
            "acl_handle": handle, "l2cap_cid": cid,
            "mode": report[1], "flags": report[2],
            "report_timestamp": int.from_bytes(report[17:21], "little"),
            "phase": report[21], "sequence": report[22], "period": report[23],
            "cycle_position": int.from_bytes(report[24:28], "little", signed=True),
            "cycle_length": int.from_bytes(report[28:32], "little"),
            "led0": report[32], "led1": report[33], "led2": report[34], "led3": report[35],
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
    ap.add_argument('--handle', type=lambda s: int(s, 0), help='ACL handle to analyse in multi-controller captures')
    args = ap.parse_args()

    rows = list(reports(args.capture))
    handles = {r['acl_handle'] for r in rows if r['acl_handle'] is not None}
    if len(handles) > 1 and args.handle is None:
        raise SystemExit(f'Multiple Bluetooth ACL handles {sorted(handles)}: specify --handle')
    if args.handle is not None:
        rows = [r for r in rows if r['acl_handle'] == args.handle]
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
