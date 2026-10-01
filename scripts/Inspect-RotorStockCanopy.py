#!/usr/bin/env python3
"""CPU-only finite stock ViewGeometry canopy/ring probe; no game or GPU.

Input RVW4 is a read-only PoseidonTools export of stock Noe. Quantized source
heights and reconstructed source planes are an inference, not installed rays.
"""
import argparse
import hashlib
import importlib.util
import json
import math
from pathlib import Path
import struct

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("rig", ROOT / "scripts/physics/inspect_stock_corpse_rig.py")
RIG = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(RIG)


def view_geometry(payload, target):
    r = RIG.Reader(payload)
    r.take(8)
    for level in range(r.count()):
        r.array(4, True)
        r.array(8, True)
        points = np.array(list(struct.iter_unpack("<3f", r.array(12))))
        r.array(12)
        r.take(48)
        for _ in range(r.count()):
            r.string()
        r.array(2, True)
        r.array(2, True)
        count = r.count()
        r.take(4)
        faces = []
        for _ in range(count):
            _, _, corners = r.value("IhB")
            faces.append(r.value("H" * corners))
        r.take(r.count() * 18)
        components = []
        for _ in range(r.count()):
            name = r.string()
            selected_faces = [v[0] for v in struct.iter_unpack("<H", r.array(2, True))]
            r.array(1, True)
            r.array(4, True)
            r.take(1)
            r.array(4, True)
            selected_vertices = [v[0] for v in struct.iter_unpack("<H", r.array(2, True))]
            r.array(1, True)
            if name.startswith("component"):
                components.append((name, selected_faces, selected_vertices))
        if level == target:
            return points, faces, components
        for _ in range(r.count()):
            r.string()
            r.string()
        for _ in range(r.count()):
            r.take(4)
            r.array(12)
        r.take(12)
        for _ in range(r.count()):
            r.string()
            r.take(56)
    raise RIG.Refused("missing physical ViewGeometry")


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--game-dir", type=Path, required=True)
    p.add_argument("--noe-rvw4", type=Path, required=True)
    p.add_argument("--out", type=Path, required=True)
    args = p.parse_args()
    archive = args.game_dir / "Dta/Data3D.pbo"
    rows, _ = RIG.archive_index(archive)
    row = next(v for v in rows if v["name"].lower() == "stan_eastc.p3d")
    with archive.open("rb") as stream:
        payload, _ = RIG.read_member(stream, row)
    actual = RIG.parse_odol7(payload)
    points, faces, components = view_geometry(payload, actual["roles"]["viewGeometry"])
    if len(points) != 80 or len(components) != 7:
        raise RIG.Refused("physical stock geometry changed")
    planes = []
    bounds = []
    for name, selected_faces, selected_vertices in components:
        centre = points[selected_vertices].mean(axis=0)
        group = []
        for face in selected_faces:
            a, b, c = points[list(faces[face][:3])]
            n = np.cross(b-a, c-a)
            length = np.linalg.norm(n)
            if not math.isfinite(length) or length < 1e-8:
                raise RIG.Refused("degenerate physical face")
            n /= length
            d = -float(n @ a)
            if float(n @ centre)+d > 0:
                n, d = -n, -d
            group.append([*n, d])
        planes.append(np.array(group))
        bounds.append(dict(name=name, vertices=len(selected_vertices), faces=len(selected_faces),
                           minimum=points[selected_vertices].min(axis=0).tolist(),
                           maximum=points[selected_vertices].max(axis=0).tolist()))
    data = args.noe_rvw4.read_bytes()
    if data[:4] != b"4WVR" or struct.unpack_from("<ii", data, 4) != (256, 256):
        raise RIG.Refused("expected stock Noe 50m grid export")
    heights = struct.unpack_from("<65536h", data, 12)

    def ground(x, z):
        xx, zz = int(x/50), int(z/50)
        u, v = x/50-xx, z/50-zz
        a, b, c, d = (heights[i]*.045 for i in (zz*256+xx, zz*256+xx+1, (zz+1)*256+xx, (zz+1)*256+xx+1))
        return a+(b-a)*u+(c-a)*v if u+v <= 1 else d+(c-d)*(1-u)+(b-d)*(1-v)

    def hit(start, end, roof):
        origin, delta = start-roof, end-start
        for group in planes:
            value = group[:, :3] @ origin + group[:, 3]
            dv = group[:, :3] @ delta
            low, high = 0., 1.
            for distance, direction in zip(value, dv):
                if abs(direction) < 1e-9:
                    if distance > 0:
                        high = -1
                        break
                elif direction < 0:
                    low = max(low, -distance/direction)
                else:
                    high = min(high, -distance/direction)
            if low <= high:
                return True
        return False

    tests = []
    for pitch in (6, 4):
        roofs = [np.array([2700+dx, 17.8150525284, 5150+dz])
                 for dx in (-2*pitch, -pitch, 0, pitch, 2*pitch)
                 for dz in (-2*pitch, -pitch, 0, pitch, 2*pitch)]
        misses = []
        for phase in range(128, 160):
            for spoke in range(8):
                h = (phase*747796405 + spoke*2891336453 + 277803737) & 0xffffffff
                jitter = ((h >> 8) & 0xffff)/65535.
                angle = 2*math.pi*(spoke+jitter*.65)/8
                distance = 12*(.7+.25*jitter)  # conservative production cap
                x, z = 2699.82+math.cos(angle)*distance, 5149.41+math.sin(angle)*distance
                g = ground(x, z)
                if not any(hit(np.array([x, g+.15, z]), np.array([2699.82, 24.8, 5149.41]), roof) for roof in roofs):
                    misses.append(dict(phase=phase, spoke=spoke, x=x, z=z, sourceGround=g))
        tests.append(dict(pitch=pitch, tents=25, rays=256, missing=len(misses), misses=misses))
    result = dict(status="source-inference-runtime-still-required", model="data3d/stan_eastC.p3d",
                  decodedSha256=hashlib.sha256(payload).hexdigest(), viewLevel=actual["roles"]["viewGeometry"],
                  components=bounds, sourceCentreGround=ground(2700,5150), cases=tests,
                  limitations="Fixed 24.8m helicopter Y and conservative radius12, quantized source ground, reconstructed planes. Actual internal pose/planes/ring phase/height and runtime hits remain authoritative.")
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(json.dumps({"status":result["status"],"sourceCentreGround":result["sourceCentreGround"],
                      "cases":[{k:v for k,v in row.items() if k != "misses"} for row in tests]}, indent=2))
    if tests[1]["missing"]:
        raise RIG.Refused("dense candidate does not cover finite source probes")


if __name__ == "__main__":
    main()
