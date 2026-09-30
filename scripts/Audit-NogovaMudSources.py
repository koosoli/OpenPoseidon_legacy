"""Read-only exact cultivated-source census from PoseidonTools' RVW4 export.

Usage: python scripts/Audit-NogovaMudSources.py <original-Noe.wrp> <export.rvw4> <report.json>
Export beforehand with PoseidonTools terrain export-rvw4. Coordinates/heights
are retail coarse-source candidates, not installed contact or shelter proof.
"""
import collections
import hashlib
import json
from pathlib import Path
import struct
import sys


def audit(wrp_path, export_path):
    data = export_path.read_bytes()
    if data[:4] != b"4WVR":
        raise ValueError("Expected PoseidonTools RVW4 export")
    width, depth = struct.unpack_from("<ii", data, 4)
    if (width, depth) != (256, 256):
        raise ValueError("Expected original Noe 256x256 source")
    cells = width * depth
    heights = struct.unpack_from(f"<{cells}h", data, 12)
    indices = struct.unpack_from(f"<{cells}H", data, 12 + cells * 2)
    table_offset = 12 + cells * 4
    textures = [data[table_offset + i * 32:table_offset + (i + 1) * 32]
                .split(b"\0")[0].decode("ascii") for i in range(512)]
    if textures[8] != "o\\pole1.paa" or textures[12] != "o\\pole2.paa":
        raise ValueError("Retail source palette identity changed; do not apply this census blindly")
    counts = collections.Counter(indices)
    candidates = []
    for z in range(2, depth - 2):
        for x in range(2, width - 2):
            palette = indices[z * width + x]
            if palette not in (8, 12):
                continue
            corners = [heights[zz * width + xx] * 0.045
                       for zz in (z, z + 1) for xx in (x, x + 1)]
            if min(corners) <= 2.0:
                continue
            candidates.append({"x": x * 50 + 25, "z": z * 50 + 25,
                               "cell": [x, z], "palette": palette, "texture": textures[palette],
                               "cornerHeights": corners,
                               "cornerRangePer50m": (max(corners) - min(corners)) / 50})
    candidates.sort(key=lambda p: (p["cornerRangePer50m"], abs(p["x"] - 6000) + abs(p["z"] - 6000)))
    return {"wrpSHA256": hashlib.sha256(wrp_path.read_bytes()).hexdigest(),
            "exportSHA256": hashlib.sha256(data).hexdigest(), "grid": [width, depth], "landGrid": 50,
            "exactCultivatedTextures": [{"id": i, "texture": textures[i], "cells": counts[i]} for i in (8, 12)],
            "limitations": "Quantized retail 50m height corners; runtime subdivision, contact, road and roof proof required",
            "flatCandidates": candidates[:20]}


if __name__ == "__main__":
    if len(sys.argv) != 4:
        raise SystemExit(__doc__)
    report = audit(Path(sys.argv[1]), Path(sys.argv[2]))
    Path(sys.argv[3]).write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print("Exact Nogova cultivated-source census:", report["exactCultivatedTextures"])
