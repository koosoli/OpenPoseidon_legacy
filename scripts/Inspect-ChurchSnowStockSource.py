#!/usr/bin/env python3
"""Bounded read-only retail kostel3 clock/roof audit and actual C++ helper probe.

No game/GPU, model mutation or redistributable stock asset output. Writes factual
JSON and a CPU probe only under --out. Uses the existing strict ODOL7 reader.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("stock_rig", ROOT / "scripts/physics/inspect_stock_corpse_rig.py")
RIG = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(RIG)
NAMES = [name + str(i) for i in range(1, 5) for name in ("hodinova", "minutova")]
EXPECTED = "14fb625d820383b5e32b1e45d563c4eeace8caaed44c2d699bd0a3f64af9484c"


def geometry(payload, actual):
    r = RIG.Reader(payload)
    r.take(8)
    rows = []
    for level in range(r.count()):
        r.array(4, True)
        r.array(8, True)
        points = list(struct.iter_unpack("<3f", r.array(12)))
        normals = list(struct.iter_unpack("<3f", r.array(12)))
        r.take(48)
        textures = [r.string() for _ in range(r.count())]
        r.array(2, True)
        r.array(2, True)
        count = r.count()
        r.take(4)
        faces = []
        for _ in range(count):
            _, texture, corners = r.value("IhB")
            faces.append((texture, r.value("H" * corners)))
        r.take(r.count() * 18)
        for _ in range(r.count()):
            r.string()
            r.array(2, True)
            r.array(1, True)
            r.array(4, True)
            r.take(1)
            r.array(4, True)
            r.array(2, True)
            r.array(1, True)
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
        lod = actual["lods"][level]
        if lod["resolution"] >= 1000:
            continue
        roof = [v for t, ids in faces if t >= 0 and textures[t].lower() == "data\\tasky_tmavsi.pac" for v in ids]
        if not roof or len(points) > 16384:
            raise RIG.Refused("missing audited roof or proof budget exceeded")
        selections = [list(lod["selections"].get(name, {})) for name in NAMES]
        rows.append(dict(level=level, resolution=lod["resolution"], points=points,
                         normals=normals, roof=roof, selections=selections))
    return rows


def literal(value):
    text = format(value, ".9g")
    return text + ("f" if "." in text or "e" in text else ".0f")


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--game-dir", type=Path, required=True)
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--compiler", default="clang++")
    args = p.parse_args()
    archive = args.game_dir / "Dta/Data3D.pbo"
    members, _ = RIG.archive_index(archive)
    member = next(row for row in members if row["name"].lower() == "kostel3.p3d")
    with archive.open("rb") as stream:
        payload, stored = RIG.read_member(stream, member)
    sha = hashlib.sha256(payload).hexdigest()
    if sha != EXPECTED:
        raise RIG.Refused("stock source differs; re-audit required")
    actual = RIG.parse_odol7(payload)  # complete independent layout/finite check
    rows = geometry(payload, actual)
    code = ['#include "' + str(ROOT / "engine/WgpuRenderer/ChurchSnowSurface.hpp").replace("\\", "/") + '"',
            '#include <cassert>\n#include <vector>\nusing namespace Poseidon;\nint main() {']
    facts = []
    for row in rows:
        points, normals, roof, sels = (row[key] for key in ("points", "normals", "roof", "selections"))
        code.append("{")
        code.append("std::vector<ObjectSnowSurface::PosePair> p={")
        for point, normal in zip(points, normals):
            values = ",".join(literal(v) for v in (*point, *(-v for v in normal)))
            code.append("{{" + values + "},{" + values + "}},")
        code.append("};std::array<std::vector<int>,8> s={" + ",".join("std::vector<int>{" + ",".join(map(str, sel)) + "}" for sel in sels) + "};")
        code.append("const auto mask=ChurchSnowSurface::BuildMask(int(p.size()),[&](int i){return int(s[i].size());},[&](int i,int j){return s[i][j];});")
        code.append("std::vector<int> roof={" + ",".join(map(str, roof)) + "};")
        code.append("auto index=[&](int i){return roof[i];};auto read=[&](int i){return p[i];};")
        code.append("assert(ChurchSnowSurface::FixedCornersRigid(mask,int(roof.size()),true,index,read));")
        code.append("for(const auto& sel:s) for(int v:sel) { assert(!ChurchSnowSurface::CornersDisjoint(mask,1,[&](int){return v;}));p[v].current[0]+=10.0f; }")
        code.append("assert(ChurchSnowSurface::FixedVerticesRigid(mask,true,read));assert(ChurchSnowSurface::FixedCornersRigid(mask,int(roof.size()),true,index,read));")
        code.append("p[roof[0]].current[4]+=0.001f;assert(!ChurchSnowSurface::FixedCornersRigid(mask,int(roof.size()),true,index,read));}")
        clock = {v for sel in sels for v in sel}
        unique_roof = sorted(set(roof))
        roof_centre = [sum(points[v][axis] for v in unique_roof)/len(unique_roof) for axis in range(3)]
        clock_centre = [sum(points[v][axis] for v in clock)/len(clock) for axis in range(3)] if clock else None
        facts.append(dict(level=row["level"], resolution=row["resolution"], vertices=len(points),
                          clockVertices=len(clock), roofTexture="data/tasky_tmavsi.pac", roofCorners=len(roof),
                          clockRoofIntersection=len(clock & set(roof)),
                          boundingSpanClockIntersection=len(clock & set(range(min(roof), max(roof)+1))),
                          minimumRoofNormalUp=min(-normals[v][1] for v in roof),
                          roofLocalCentre=roof_centre, clockLocalCentre=clock_centre))
    code.append("}")
    args.out.mkdir(parents=True, exist_ok=True)
    cpp, exe = args.out / "actual-stock-helper.cpp", args.out / "actual-stock-helper.exe"
    cpp.write_text("\n".join(code), encoding="utf-8")
    subprocess.run([args.compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", str(cpp), "-o", str(exe)], check=True)
    subprocess.run([str(exe.resolve())], check=True)
    result = dict(status="source-cpu-proof-only-runtime-pending", model="data3d/kostel3.p3d",
                  member=member, storedSha256=stored, decodedSha256=sha,
                  allowAnimation=actual["allowAnimation"], visualLods=facts,
                  actualCppHelper="PASS-original-roof/moving-clock/veto/mismatched-roof-all-five-LODs",
                  limitations="WRP owner 4867 identity separate; source proof cannot establish installed admission, physical exposure or appearance.")
    (args.out / "source-proof.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
