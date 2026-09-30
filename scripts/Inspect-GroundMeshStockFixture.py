#!/usr/bin/env python3
"""Read-only bounded stock concrete fixture audit; never runs the game.

Writes only JSON and a config-only PBO under --out. The private class is NOT a
stock class: it references the untouched retail model through NonStrategic.
Source facts cannot establish installed receiver admission or pixel response.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct

SPEC = importlib.util.spec_from_file_location(
    "stock_rig", Path(__file__).parent / "physics/inspect_stock_corpse_rig.py")
RIG = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(RIG)

MODEL = "Molo_beton.p3d"
# Decoded ODOL7, not the compressed PBO member. Reject different assets rather
# than silently seating an unknown variant at an assumed height.
DECODED_SHA = "ad8192875966fec2e0660b2c80087af21fde029b541aefdb811615b0ffd61e51"
CLASS = "GroundPuddleConcreteFixture"


def source_lods(payload):
    r = RIG.Reader(payload)
    if r.take(4) != b"ODOL" or r.u32() != 7:
        raise RIG.Refused("only ODOL7 accepted")
    count = r.count()
    if not 1 <= count <= 100:
        raise RIG.Refused("LOD cap")
    lods = []
    for _ in range(count):
        clips = [v[0] for v in struct.iter_unpack("<I", r.array(4, True))]
        r.array(8, True)
        points = list(struct.iter_unpack("<3f", r.array(12)))
        normals = list(struct.iter_unpack("<3f", r.array(12)))
        r.take(48)
        textures = [r.string() for _ in range(r.count())]
        r.array(2, True)
        r.array(2, True)
        faces = []
        face_count = r.count()
        r.take(4)
        for _ in range(face_count):
            flags, texture, corners = r.value("IhB")
            if corners not in (3, 4):
                raise RIG.Refused("face corner count")
            indices = r.value("H" * corners)
            if any(i >= len(points) for i in indices):
                raise RIG.Refused("face index")
            faces.append((flags, texture, indices))
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
        properties = [(r.string(), r.string()) for _ in range(r.count())]
        frames = r.count()
        for _ in range(frames):
            r.take(4)
            r.array(12)
        r.take(12)
        proxies = r.count()
        for _ in range(proxies):
            r.string()
            r.take(56)
        if len(clips) != len(points) or len(normals) != len(points) or not points:
            raise RIG.Refused("incomplete vertex arrays")
        top = max(p[1] for p in points)
        top_faces = [(f, t, ids) for f, t, ids in faces
                     if all(abs(points[i][1] - top) < 1e-4 for i in ids)]
        # ODOL source normals are negated by engine model loading.
        source_up = sum(all(normals[i][1] < -.995 for i in ids)
                        for _, _, ids in top_faces)
        lods.append(dict(pointCount=len(points),
                         landFlags=sorted({c & 0xf00 for c in clips}),
                         faceFlags=sorted({f for f, _, _ in faces}),
                         topFaceCount=len(top_faces), upwardTopFaceCount=source_up,
                         topFaceFlags=sorted({f for f, _, _ in top_faces}),
                         topTextures=sorted({textures[t] for _, t, _ in top_faces
                                             if 0 <= t < len(textures)}),
                         topY=top, frames=frames, proxies=proxies,
                         properties=properties))
    # Full layout/trailing bytes and finite values are checked independently by
    # the repository's actual ODOL7 reader, not guessed from this partial walk.
    actual = RIG.parse_odol7(payload)
    for row, loaded in zip(lods, actual["lods"]):
        row["resolution"] = loaded["resolution"]
        row["minimum"] = loaded["points"].min(axis=0).tolist()
        row["maximum"] = loaded["points"].max(axis=0).tolist()
    return actual, [l for l in lods if l["resolution"] < 1000]


def pack_config(config):
    data = config.encode("ascii")
    # Plain PBO entry, no copied model or altered stock content.
    return (b"config.cpp\0" + struct.pack("<5I", 0, len(data), 0, 0, len(data))
            + bytes(21) + data)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--game-dir", type=Path, required=True)
    p.add_argument("--out", type=Path, required=True)
    args = p.parse_args()
    archive = args.game_dir / "Dta/Data3D.pbo"
    rows, total = RIG.archive_index(archive)
    hits = [r for r in rows if r["name"].lower() == MODEL.lower()]
    if len(hits) != 1:
        raise RIG.Refused("no unique stock concrete member")
    with archive.open("rb") as stream:
        payload, stored_sha = RIG.read_member(stream, hits[0])
    decoded_sha = hashlib.sha256(payload).hexdigest()
    if decoded_sha != DECODED_SHA:
        raise RIG.Refused("stock concrete payload differs from audited version")
    actual, lods = source_lods(payload)
    forbidden_face = 0x8 | 0x10 | 0x40 | 0x100 | 0x200 | 0x400 | 0x800
    if (actual["allowAnimation"] or len(lods) != 3 or
        any(l["landFlags"] != [0] or l["frames"] or l["proxies"] or
            not l["topFaceCount"] or l["upwardTopFaceCount"] != l["topFaceCount"] or
            any(f & forbidden_face for f in l["topFaceFlags"]) or
            abs(l["topY"]-2.4916980266571045) > 1e-6 for l in lods)):
        raise RIG.Refused("stock concrete no longer satisfies fixture source contract")
    args.out.mkdir(parents=True, exist_ok=True)
    addon = args.out / "private-mod/AddOns/ground_puddle_fixture.pbo"
    addon.parent.mkdir(parents=True, exist_ok=True)
    config = '''// Private test alias; never a claimed stock class.
class CfgPatches { class GroundPuddleFixture {
 units[]={"GroundPuddleConcreteFixture"}; weapons[]={};
 requiredVersion=1.0; requiredAddons[]={};
}; };
class CfgVehicles { class NonStrategic;
 class GroundPuddleConcreteFixture: NonStrategic {
  scope=2; displayName="Ground puddle fixture (stock concrete geometry)";
  model="\\data3d\\Molo_beton.p3d";
 };
};
'''
    addon.write_bytes(pack_config(config))
    result = dict(status="source-audited-runtime-unproven", privateClass=CLASS,
                  classAuthority="private alias of stock NonStrategic/Building; installed admission required",
                  stockModel="data3d/molo_beton.p3d", archive=str(archive),
                  archiveBytes=total, member=hits[0], storedSha256=stored_sha,
                  decodedSha256=decoded_sha, allowAnimation=actual["allowAnimation"],
                  boundingCenter=actual["boundingCenter"], visualLods=lods,
                  modelTopY=lods[0]["topY"], privateMod=str(addon.parent.parent),
                  addon=str(addon), addonSha256=hashlib.sha256(addon.read_bytes()).hexdigest(),
                  cutoutCoverage="not established; top source flags are solid depth-writing",
                  limitations="No named stock class found. Source flags do not prove installed model recentering, receiver tag, exposure or appearance.")
    (args.out / "stock-source.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
