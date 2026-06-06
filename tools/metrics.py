#!/usr/bin/env python3
# Copyright (c) 2026 Vladimir Lysyy (mrbald@github)
# SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-uFW-Commercial
"""
Read a uFW telemetry file (ufw/core/metrics/) from OUTSIDE the process — the
cold-tier consumer. Mirrors the on-file layout field for field; if this script can
parse it, any sidecar (prometheus exposition, OTel) can.

Usage:
    tools/metrics.py dump  <file>             one snapshot, all gauges
    tools/metrics.py watch <file> [-i SECS]   refresh in place (default 1s)

No third-party deps. Seqlock-consistent reads (retry while a writer is mid-update).
"""
from __future__ import annotations

import argparse
import math
import mmap
import struct
import sys
import time

MAGIC = 0x5854454D57465500  # "\0UFWMETX" little-endian
HEADER_FMT = "<Q4I3Q16x"  # magic, version, cap, count, reserved, arena off/cap/used
HEADER_SIZE = struct.calcsize(HEADER_FMT)
ENTRY_FMT = "<112sIIQ"  # name, type, payload_len, payload_offset
ENTRY_SIZE = struct.calcsize(ENTRY_FMT)

T_COUNTER, T_VAL_I64, T_VAL_F64, T_VAL_STR, T_SERIES = 1, 2, 3, 4, 5

HIST_MINOR_BITS = 4
HIST_MINORS = 1 << HIST_MINOR_BITS
HIST_BUCKETS = (64 - HIST_MINOR_BITS + 1) * HIST_MINORS  # 976
WORST_N = 8
SERIES_FMT = f"<5Q3d{WORST_N}Q{WORST_N}Q{HIST_BUCKETS}Q"


def bucket_lo(idx: int) -> int:
    """Lower edge of histogram bucket idx (mirror of hist_bucket_index)."""
    if idx < HIST_MINORS:
        return idx
    major = idx // HIST_MINORS + (HIST_MINOR_BITS - 1)
    minor = idx % HIST_MINORS
    return (HIST_MINORS + minor) << (major - HIST_MINOR_BITS)


def percentile(buckets: list[int], count: int, q: float) -> int:
    """Approximate quantile from cumulative-per-bucket counts (lower edge)."""
    if count == 0:
        return 0
    rank = math.ceil(count * q)
    seen = 0
    for i, n in enumerate(buckets):
        seen += n
        if seen >= rank:
            return bucket_lo(i)
    return bucket_lo(HIST_BUCKETS - 1)


def read_stable(mm, off: int, size: int, seq_off: int | None) -> bytes:
    """Seqlock read: retry while the writer holds an odd seq or moves it."""
    for _ in range(1000):
        if seq_off is None:
            return mm[off:off + size]
        (s0,) = struct.unpack_from("<Q", mm, seq_off)
        if s0 % 2:
            continue
        data = bytes(mm[off:off + size])
        (s1,) = struct.unpack_from("<Q", mm, seq_off)
        if s0 == s1:
            return data
    return bytes(mm[off:off + size])  # writer gone wild; take the torn read


def load(path: str):
    with open(path, "rb") as f:
        mm = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
    magic, version, cap, count, _res, arena_off, _acap, _aused = struct.unpack_from(
        HEADER_FMT, mm, 0)
    if magic != MAGIC:
        sys.exit(f"not a uFW metrics file (magic {magic:#x}): {path}")
    if version != 1:
        sys.exit(f"unsupported version {version}: {path}")
    gauges = []
    for i in range(count):
        name_b, gtype, plen, poff = struct.unpack_from(
            ENTRY_FMT, mm, HEADER_SIZE + i * ENTRY_SIZE)
        name = name_b.split(b"\0", 1)[0].decode()
        gauges.append((name, gtype, poff, plen))
    return mm, gauges


def render(mm, gauges) -> list[str]:
    lines = []
    for name, gtype, poff, plen in gauges:
        if gtype == T_COUNTER:
            (v,) = struct.unpack_from("<Q", mm, poff)
            lines.append(f"{name} = {v}")
        elif gtype == T_VAL_I64:
            (v,) = struct.unpack_from("<q", mm, poff)
            lines.append(f"{name} = {v}")
        elif gtype == T_VAL_F64:
            (v,) = struct.unpack_from("<d", mm, poff)
            lines.append(f"{name} = {v:.3f}")
        elif gtype == T_VAL_STR:
            data = read_stable(mm, poff, plen, seq_off=poff)
            s = data[8:].split(b"\0", 1)[0].decode(errors="replace")
            lines.append(f'{name} = "{s}"')
        elif gtype == T_SERIES:
            data = read_stable(mm, poff, plen, seq_off=poff)
            fields = struct.unpack(SERIES_FMT, data)
            _seq, count, total, lo, hi = fields[:5]
            mean, m2, ewma = fields[5:8]
            worst_vals = fields[8:8 + WORST_N]
            buckets = list(fields[8 + 2 * WORST_N:])
            if count == 0:
                lines.append(f"{name}: no samples")
                continue
            std = math.sqrt(m2 / count) if count > 1 else 0.0
            p50 = percentile(buckets, count, 0.50)
            p99 = percentile(buckets, count, 0.99)
            p999 = percentile(buckets, count, 0.999)
            worst = sorted((w for w in worst_vals if w), reverse=True)
            lines.append(
                f"{name}: count={count} sum={total} min={lo} max={hi} "
                f"mean={mean:.1f} std={std:.1f} ewma={ewma:.1f} "
                f"~p50={p50} ~p99={p99} ~p99.9={p999} worst={worst}")
        else:
            lines.append(f"{name}: unknown type {gtype}")
    return lines


def main() -> int:
    p = argparse.ArgumentParser(description="read a uFW telemetry file")
    p.add_argument("mode", choices=["dump", "watch"])
    p.add_argument("file")
    p.add_argument("-i", "--interval", type=float, default=1.0)
    args = p.parse_args()

    mm, gauges = load(args.file)
    if args.mode == "dump":
        print("\n".join(render(mm, gauges)))
        return 0
    try:
        while True:
            out = render(mm, gauges)
            print("\033[2J\033[H", end="")  # clear + home
            print(f"{args.file} — {len(gauges)} gauges — {time.strftime('%H:%M:%S')}")
            print("\n".join(out))
            time.sleep(args.interval)
    except KeyboardInterrupt:
        return 0


if __name__ == "__main__":
    sys.exit(main())
