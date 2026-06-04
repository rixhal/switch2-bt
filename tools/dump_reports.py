#!/usr/bin/env python3
"""
dump_reports.py — Analyze JSONL report dumps from switch2_ble_probe.py

Parses JSONL files and produces:
  - Per-report hex display with changed bytes highlighted
  - Summary statistics (report count, frequency, byte variance)
  - Optional: diff between any two reports

Usage:
  python3 tools/dump_reports.py reports.jsonl
  python3 tools/dump_reports.py reports.jsonl --diff 1 5
  python3 tools/dump_reports.py reports.jsonl --byte-frequency
"""

from __future__ import annotations

import argparse
import json
import sys
from collections import Counter
from pathlib import Path
from typing import List, Dict, Optional


def load_reports(path: Path) -> List[dict]:
    """Load JSONL report file. Returns list of records."""
    records = []
    with path.open() as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                records.append(json.loads(line))
            except json.JSONDecodeError as e:
                print(f"WARNING: Skipping malformed line: {e}", file=sys.stderr)
    return records


def hex_pretty(data_hex: str, group_size: int = 8) -> str:
    """Format hex string into grouped display."""
    pairs = [data_hex[i:i+2] for i in range(0, len(data_hex), 2)]
    chunks = [" ".join(pairs[i:i+group_size]) for i in range(0, len(pairs), group_size)]
    return "  ".join(chunks)


def show_reports(records: List[dict], highlight: Optional[set] = None):
    """Display report summaries with hex dump."""
    highlight = highlight or set()
    for rec in records:
        n = rec["report_number"]
        ts = rec["timestamp"]
        raw = bytes.fromhex(rec["hex"])
        changed = rec.get("changed_bytes", [])

        print(f"\n── Report #{n} ── t={ts:.3f}s ── {len(raw)} bytes ── {len(changed)} changed ──")

        # Highlight changed bytes
        if changed:
            for start in range(0, len(raw), 16):
                chunk = raw[start:start+16]
                offset = start
                hex_parts = []
                ascii_parts = []
                for i, b in enumerate(chunk):
                    byte_pos = offset + i
                    if byte_pos in changed:
                        hex_parts.append(f"\033[1;33m{b:02x}\033[0m")  # bright yellow
                        ascii_parts.append(f"\033[1;33m{chr(b) if 32 <= b < 127 else '.'}\033[0m")
                    elif byte_pos in highlight:
                        hex_parts.append(f"\033[1;36m{b:02x}\033[0m")  # cyan
                        ascii_parts.append(f"\033[1;36m{chr(b) if 32 <= b < 127 else '.'}\033[0m")
                    else:
                        hex_parts.append(f"{b:02x}")
                        ascii_parts.append(chr(b) if 32 <= b < 127 else '.')
                print(f"  {offset:4d}: {' '.join(hex_parts):<48s}  {''.join(ascii_parts)}")
        else:
            # Just hexdump
            for start in range(0, len(raw), 16):
                chunk = raw[start:start+16]
                offset = start
                hex_part = " ".join(f"{b:02x}" for b in chunk)
                ascii_part = "".join(chr(b) if 32 <= b < 127 else '.' for b in chunk)
                print(f"  {offset:4d}: {hex_part:<48s}  {ascii_part}")


def diff_reports(records: List[dict], idx_a: int, idx_b: int):
    """Show diff between two reports."""
    if idx_a >= len(records) or idx_b >= len(records):
        print(f"ERROR: indices out of range (max {len(records)-1})")
        return

    a = records[idx_a]
    b = records[idx_b]
    raw_a = bytes.fromhex(a["hex"])
    raw_b = bytes.fromhex(b["hex"])

    print(f"\nDiff: Report #{a['report_number']} ↔ #{b['report_number']}")
    print(f"  Length: {len(raw_a)} ↔ {len(raw_b)}")

    max_len = max(len(raw_a), len(raw_b))
    diffs = []
    for i in range(max_len):
        byte_a = raw_a[i] if i < len(raw_a) else None
        byte_b = raw_b[i] if i < len(raw_b) else None
        if byte_a != byte_b:
            diffs.append((i, byte_a, byte_b))

    if diffs:
        print(f"  Changed bytes: {len(diffs)}")
        for pos, ba, bb in diffs:
            a_hex = f"0x{ba:02x}" if ba is not None else "----"
            b_hex = f"0x{bb:02x}" if bb is not None else "----"
            print(f"    [{pos:3d}] {a_hex} → {b_hex}")
    else:
        print("  No changes.")


def byte_frequency(records: List[dict]):
    """Show which byte positions change most frequently."""
    position_changes: Dict[int, int] = Counter()
    position_values: Dict[int, Counter] = {}

    prev_data = None
    for rec in records:
        data = bytes.fromhex(rec["hex"])
        for i, b in enumerate(data):
            if i not in position_values:
                position_values[i] = Counter()
            position_values[i][b] += 1

            if prev_data and i < len(prev_data) and prev_data[i] != b:
                position_changes[i] += 1
        prev_data = data

    print("\n── Byte Position Change Frequency ──")
    print(f"{'Pos':>4s}  {'Changes':>7s}  {'Unique':>6s}  {'Values'}")
    for pos in sorted(position_changes.keys(), key=lambda p: -position_changes[p]):
        changes = position_changes[pos]
        values = position_values.get(pos, Counter())
        unique = len(values)
        top_vals = ", ".join(f"0x{v:02x}({c})" for v, c in values.most_common(5))
        print(f"  {pos:3d}  {changes:7d}  {unique:6d}  {top_vals}")


def summary(records: List[dict]):
    """Print summary statistics."""
    if not records:
        print("No reports to summarize.")
        return

    first = records[0]
    last = records[-1]
    duration = last["timestamp"] - first["timestamp"]
    rate = len(records) / duration if duration > 0 else 0

    # Identify static (never-changing) and dynamic bytes
    all_data = [bytes.fromhex(r["hex"]) for r in records]
    max_len = max(len(d) for d in all_data) if all_data else 0

    static = []
    dynamic = []
    for pos in range(max_len):
        values = set()
        for d in all_data:
            if pos < len(d):
                values.add(d[pos])
        if len(values) == 1:
            static.append(pos)
        else:
            dynamic.append(pos)

    print(f"\n── Summary ──")
    print(f"  Reports:   {len(records)}")
    print(f"  Duration:  {duration:.3f}s")
    print(f"  Rate:      {rate:.1f} reports/s")
    print(f"  Avg size:  {sum(len(bytes.fromhex(r['hex'])) for r in records) / len(records):.0f} bytes")
    print(f"  Static bytes:   {len(static)} — {static}")
    print(f"  Dynamic bytes:  {len(dynamic)} — {dynamic}")


def main():
    p = argparse.ArgumentParser(description="Analyze Switch 2 BLE report dumps")
    p.add_argument("jsonl_file", type=Path, help="JSONL file from switch2_ble_probe.py")
    p.add_argument("--diff", type=int, nargs=2, metavar=("A", "B"),
                   help="Diff two reports by index (0-based)")
    p.add_argument("--byte-frequency", action="store_true",
                   help="Show per-byte change frequency table")
    p.add_argument("--all", action="store_true",
                   help="Show all reports (may be very long)")
    args = p.parse_args()

    records = load_reports(args.jsonl_file)
    print(f"Loaded {len(records)} reports from {args.jsonl_file}")

    if args.diff:
        diff_reports(records, args.diff[0], args.diff[1])

    if args.all:
        show_reports(records)

    if args.byte_frequency:
        byte_frequency(records)

    summary(records)


if __name__ == "__main__":
    main()
