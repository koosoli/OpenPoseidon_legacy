"""Convert `PoseidonTools terrain objects` output into a compact binary the
farfield harness can load: x, y, z as f32 plus a one-byte class.

The class split is what the far-field question turns on. Vegetation is
statistically interchangeable and can be regenerated from a density field; a
structure has to be exactly where the author put it. They therefore want
different representations, and the first thing needed is an honest count of each.

    python mkplacements.py everon_objects.txt everon.bin
"""

import re
import struct
import sys

LINE = re.compile(r"^(\d+)\s+(\S*?\.p3d)\s*(-?[\d.]+)\s+(-?[\d.]+)\s+(-?[\d.]+)\s*$")

VEG = ("/vegetation/", "/plants/", "/tree", "/bush", "/grass")
STRUCT = ("/structures/", "/buildings/", "/industrial/", "/military/", "/houses/")


def classify(path):
    q = path.lower().replace("\\", "/")
    if any(k in q for k in VEG):
        return 0
    if any(k in q for k in STRUCT):
        return 1
    return 2


def main():
    src, dst = sys.argv[1], sys.argv[2]
    counts = [0, 0, 0]
    skipped = 0
    lo = [1e30] * 3
    hi = [-1e30] * 3

    with open(src, "r", errors="replace") as f, open(dst, "wb") as o:
        o.write(struct.pack("<I", 0))  # count, patched at the end
        for line in f:
            m = LINE.match(line.rstrip("\n"))
            if not m:
                skipped += 1
                continue
            _, model, x, y, z = m.groups()
            x, y, z = float(x), float(y), float(z)
            counts[classify(model)] += 1
            for i, v in enumerate((x, y, z)):
                lo[i] = min(lo[i], v)
                hi[i] = max(hi[i], v)
            o.write(struct.pack("<fffB", x, y, z, classify(model)))
        total = sum(counts)
        o.seek(0)
        o.write(struct.pack("<I", total))

    print(f"placements   {total:,}  (skipped {skipped} non-data lines)")
    for name, i in (("vegetation", 0), ("structures", 1), ("other", 2)):
        print(f"  {name:<11}{counts[i]:,} ({counts[i] / total * 100:.1f}%)")
    print(
        f"  extent     x {lo[0]:.0f}..{hi[0]:.0f}  "
        f"y {lo[1]:.0f}..{hi[1]:.0f}  z {lo[2]:.0f}..{hi[2]:.0f}"
    )


if __name__ == "__main__":
    main()
