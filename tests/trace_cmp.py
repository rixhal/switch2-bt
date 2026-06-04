#!/usr/bin/env python3
"""
trace_cmp.py — Compare two HCI/SMP packet traces (btmon hex format or raw hexdump).
Finds first divergence before disconnect reason 0x13.

Usage: python3 tests/trace_cmp.py trace_a.txt trace_b.txt
"""
import sys, re, os

def parse_hexdump(text):
    """Extract hex bytes from btmon-style or raw hexdump output."""
    packets = []
    current = []
    for line in text.split('\n'):
        # btmon format: "> HCI Event: ..." or raw hexdump
        if line.startswith('> HCI') or line.startswith('< HCI'):
            if current:
                packets.append(bytes(current))
                current = []
            continue
        # Extract hex bytes: patterns like "02 40 00" or "0x02, 0x40"
        for m in re.finditer(r'(?:0x)?([0-9a-fA-F]{2})\b', line):
            current.append(int(m.group(1), 16))
    if current:
        packets.append(bytes(current))
    return packets

def find_first_diff(a, b):
    """Find first byte-level difference between two traces."""
    for i, (pa, pb) in enumerate(zip(a, b)):
        if pa != pb:
            # Find exact differing byte
            for j in range(min(len(pa), len(pb))):
                if pa[j] != pb[j]:
                    return i, j, pa, pb
            if len(pa) != len(pb):
                return i, min(len(pa), len(pb)), pa, pb
    if len(a) != len(b):
        return min(len(a), len(b)), 0, b'', b''
    return None, None, None, None

def main():
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <trace_a.txt> <trace_b.txt>")
        print("  trace_a: sw2d_final golden path")
        print("  trace_b: BTstack fail path")
        sys.exit(1)

    for i, path in enumerate([sys.argv[1], sys.argv[2]]):
        if not os.path.exists(path):
            print(f"ERROR: {path} not found")
            sys.exit(1)

    with open(sys.argv[1]) as f:
        a_packets = parse_hexdump(f.read())
    with open(sys.argv[2]) as f:
        b_packets = parse_hexdump(f.read())

    print(f"Trace A: {len(a_packets)} packets, {sum(len(p) for p in a_packets)} bytes")
    print(f"Trace B: {len(b_packets)} packets, {sum(len(p) for p in b_packets)} bytes")

    pi, bi, pa, pb = find_first_diff(a_packets, b_packets)

    if pi is None:
        print("Traces identical at byte level.")
        return 0

    print(f"\nFIRST DIVERGENCE at packet {pi}, byte {bi}:")
    print(f"  A[{pi}][{bi}] = 0x{pa[bi]:02x}  (len={len(pa)})")
    print(f"  B[{pi}][{bi}] = 0x{pb[bi]:02x}  (len={len(pb)})")
    print(f"\n  Packet A[{pi}]: {' '.join(f'{b:02x}' for b in pa)}")
    print(f"  Packet B[{pi}]: {' '.join(f'{b:02x}' for b in pb)}")

    # Check for disconnect 0x13 in trace B
    for i, p in enumerate(b_packets):
        if len(p) >= 4 and p[0] == 0x01 and p[1] == 0x13:
            print(f"\n  Disconnect 0x13 found in B at packet {i}: {' '.join(f'{b:02x}' for b in p)}")
            # Show context: 5 packets before
            start = max(0, i-5)
            print(f"  Context (packets {start}-{i}):")
            for j in range(start, i+1):
                label = ">>>" if j == i else "   "
                print(f"  {label} B[{j}]: {' '.join(f'{b:02x}' for b in b_packets[j][:32])}{'...' if len(b_packets[j])>32 else ''}")
            break

    return 1

if __name__ == '__main__':
    sys.exit(main())
