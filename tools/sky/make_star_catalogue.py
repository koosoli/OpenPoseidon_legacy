#!/usr/bin/env python3
"""Build `resources/sky/bright_stars.pstar` for the wgpu night sky.

The renderer (engine/WgpuRenderer/rust/src/sky/starcat.rs) reads a trivial binary file so that
swapping the placeholder for a real catalogue is a DATA change and not a code change:

    offset  type      meaning
    0       u8[4]     magic "PSTR"
    4       u32 LE    version (1)
    8       u32 LE    record count
    12      u32 LE    flags (reserved, 0)
    16      record[]  16 bytes each, four f32 LE:
                        ra_deg   right ascension, degrees, J2000, [0,360)
                        dec_deg  declination,     degrees, J2000, [-90,90]
                        vmag     visual magnitude (lower = brighter)
                        bv       B-V colour index (blue ~ -0.3, red ~ +1.8)

Two sources are understood:

  --hyg <hygdata_v3.csv>   The HYG database (CC BY-SA 4.0). Columns used: ra (HOURS), dec, mag, ci.
  --bsc <catalog>          Yale Bright Star Catalogue, 5th ed., fixed-width ASCII (public domain,
                           via CDS/ADC V/50). Columns per the published byte-by-byte description.

and one that needs no download at all:

  --placeholder            Emit the same generated field the renderer falls back to. Positions
                           are NOT real; this exists so the pipeline can be exercised.

LICENCE NOTE. HYG is CC BY-SA 4.0 and would need attribution in THIRD_PARTY_NOTICES.md and a
compatible redistribution story. The Yale BSC as distributed by CDS/ADC is public domain and is
the cleaner choice for a GPL project that ships the data. Whatever is chosen, record it before
committing the .pstar.
"""

import argparse
import math
import struct
import sys
from pathlib import Path

MAG_LIMIT_DEFAULT = 6.5


def write_pstar(path: Path, records):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("wb") as f:
        f.write(b"PSTR")
        f.write(struct.pack("<III", 1, len(records), 0))
        for ra, dec, mag, bv in records:
            f.write(struct.pack("<ffff", ra, dec, mag, bv))
    print(f"wrote {path} - {len(records)} stars, {path.stat().st_size} bytes")


def from_hyg(csv_path: Path, mag_limit: float):
    import csv

    out = []
    with csv_path.open(newline="", encoding="utf-8") as f:
        for row in csv.DictReader(f):
            try:
                mag = float(row["mag"])
            except (KeyError, ValueError):
                continue
            if mag > mag_limit:
                continue
            try:
                ra_h = float(row["ra"])          # HOURS in HYG, not degrees
                dec = float(row["dec"])
            except (KeyError, ValueError):
                continue
            # Missing colour index is common for faint entries; 0.6 is a solar-ish default and
            # renders near-white, which is the least wrong guess.
            try:
                bv = float(row["ci"])
            except (KeyError, ValueError, TypeError):
                bv = 0.6
            out.append((ra_h * 15.0 % 360.0, dec, mag, bv))
    return out


def from_bsc(path: Path, mag_limit: float):
    """Yale BSC5 fixed-width. Byte ranges (1-based, per the ADC description):
    RAh 76-77, RAm 78-79, RAs 80-83, DE- 84, DEd 85-86, DEm 87-88, DEs 89-90,
    Vmag 103-107, B-V 110-114."""
    out = []
    for line in path.open(encoding="latin-1"):
        if len(line) < 115:
            continue
        try:
            rah = float(line[75:77])
            ram = float(line[77:79])
            ras = float(line[79:83])
            sign = -1.0 if line[83] == "-" else 1.0
            ded = float(line[84:86])
            dem = float(line[86:88])
            des = float(line[88:90])
            vmag = float(line[102:107])
        except ValueError:
            continue  # novae and other entries with blank astrometry
        try:
            bv = float(line[109:114])
        except ValueError:
            bv = 0.6
        if vmag > mag_limit:
            continue
        ra = (rah + ram / 60.0 + ras / 3600.0) * 15.0
        dec = sign * (ded + dem / 60.0 + des / 3600.0)
        out.append((ra % 360.0, dec, vmag, bv))
    return out


def placeholder(count: int, mag_limit: float):
    """Mirror of `generate_placeholder` in starcat.rs, same LCG and same constants, so the file
    and the built-in fallback are the same field."""
    state = 0x9E3779B97F4A7C15
    mask = (1 << 64) - 1

    def nxt():
        nonlocal state
        state = (state * 6364136223846793005 + 1442695040888963407) & mask
        return ((state >> 33) & 0xFFFFFFFF) / float(0x7FFFFFFF)

    out = []
    for _ in range(count):
        ra = nxt() * 360.0
        dec = math.degrees(math.asin(max(-1.0, min(1.0, 2.0 * nxt() - 1.0))))
        u = max(nxt(), 1e-6)
        vmag = max(-1.5, min(mag_limit, mag_limit + math.log10(u) / 0.6))
        bv = -0.3 + 2.1 * (0.5 * (nxt() + nxt()))
        out.append((ra, dec, vmag, bv))
    return out


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--hyg", type=Path, help="hygdata_v3.csv")
    src.add_argument("--bsc", type=Path, help="Yale BSC5 fixed-width catalog file")
    src.add_argument("--placeholder", action="store_true", help="generated field, positions not real")
    ap.add_argument("--count", type=int, default=1200, help="placeholder star count")
    ap.add_argument("--mag-limit", type=float, default=MAG_LIMIT_DEFAULT,
                    help="drop stars fainter than this (default 6.5, the naked-eye limit)")
    ap.add_argument("-o", "--out", type=Path, default=Path("resources/sky/bright_stars.pstar"))
    args = ap.parse_args(argv)

    if args.placeholder:
        recs = placeholder(args.count, args.mag_limit)
    elif args.hyg:
        recs = from_hyg(args.hyg, args.mag_limit)
    else:
        recs = from_bsc(args.bsc, args.mag_limit)

    if not recs:
        print("no records parsed - check the source file and its format", file=sys.stderr)
        return 1
    # Brightest first: not required by the reader, but it makes a truncated file degrade into a
    # dimmer sky rather than a randomly holed one.
    recs.sort(key=lambda r: r[2])
    write_pstar(args.out, recs)
    print(f"brightest {recs[0][2]:.2f} mag, faintest {recs[-1][2]:.2f} mag")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
