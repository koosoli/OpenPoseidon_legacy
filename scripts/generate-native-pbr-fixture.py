"""Owned, synthetic native-channel material fixture; requires only Python's stdlib.

No Reforger files are extracted or bundled. Four spheres share identical BCR/NMO
texels and geometry; only their authored Basic roughness/metalness scales differ.
"""
import argparse
import json
import math
from pathlib import Path
import struct


def dds(path, pixel):
    # The existing EDDS loader also accepts a standard DDS payload. Keep RGBA
    # masks explicit so BCR alpha and NMO B/A remain independently observable.
    header = [124, 0x100F, 16, 16, 64, 0, 1] + [0] * 11
    header += [32, 0x41, 0, 32, 0xFF, 0xFF00, 0xFF0000, 0xFF000000]
    header += [0x1000, 0, 0, 0, 0]
    path.write_bytes(b"DDS " + struct.pack("<31I", *header) + bytes(pixel) * 256)


def create_fixture(root):
    root.mkdir(parents=True, exist_ok=True)
    bcr, nmo = root / "probe_BCR.edds", root / "probe_NMO.edds"
    dds(bcr, (155, 95, 45, 160))
    dds(nmo, (128, 128, 230, 255))
    arms = [("rough_metal", 1.0, 1.0), ("smooth_metal", 0.3, 1.0),
            ("rough_dielectric", 1.0, 0.0), ("smooth_dielectric", 0.3, 0.0)]
    points, normals, uvs, faces = [], [], [], []
    for sphere, (name, roughness, metalness) in enumerate(arms):
        material = root / (name + ".emat")
        material.write_text(
            'MatPBRBasic {\n Cull none\n Color 1 1 1 1\n'
            f' RoughnessScale {roughness}\n MetalnessScale {metalness}\n'
            f' BCRMap "{{0000000000000001}}{bcr.as_posix()}"\n'
            f' NMOMap "{{0000000000000002}}{nmo.as_posix()}"\n}}\n',
            encoding="ascii")
        offset = len(points)
        rings, segments = 12, 24
        for ring in range(rings + 1):
            phi = math.pi * ring / rings
            for segment in range(segments + 1):
                theta = math.tau * segment / segments
                normal = (math.sin(phi) * math.cos(theta), math.cos(phi),
                          math.sin(phi) * math.sin(theta))
                points.append((normal[0] * 1.2 + (sphere - 1.5) * 3.1,
                               normal[1] * 1.2, normal[2] * 1.2))
                normals.append(normal)
                uvs.append((segment / segments, ring / rings))
        for ring in range(rings):
            for segment in range(segments):
                a = offset + ring * (segments + 1) + segment
                b = a + segments + 1
                # Triangle winding follows the outward sphere normal.
                if ring > 0:
                    faces.append(((a, a + 1, b), material))
                if ring < rings - 1:
                    faces.append(((a + 1, b + 1, b), material))
    lod = bytearray(b"P3DM" + struct.pack("<6I", 28, 256, len(points), len(normals), len(faces), 0))
    for point in points:
        lod.extend(struct.pack("<3fI", *point, 0))
    for normal in normals:
        lod.extend(struct.pack("<3f", *normal))
    for vertices, material in faces:
        lod.extend(struct.pack("<I", 3))
        for vertex in (*vertices, 0):
            lod.extend(struct.pack("<II2f", vertex, vertex, *uvs[vertex]))
        lod.extend(struct.pack("<I", 0))
        lod.extend(bcr.as_posix().encode("ascii") + b"\0")
        lod.extend(material.as_posix().encode("ascii") + b"\0")
    lod.extend(b"TAGG\x01#EndOfFile#\0" + struct.pack("<If", 0, 1.0))
    model = root / "native-pbr-spheres.p3d"
    model.write_bytes(b"MLOD" + struct.pack("<II", 0x101, 1) + lod)
    terrain = bytearray(b"4WVR" + struct.pack("<II", 256, 256))
    terrain.extend(struct.pack("<h", 5000) * (256 * 256))
    terrain.extend(struct.pack("<H", 1) * (256 * 256))
    names = bytearray(512 * 32 + 128)
    names[:len(b"landtext\\mo.pac")] = b"landtext\\mo.pac"
    names[32:32+len(b"o\\pt.paa")] = b"o\\pt.paa"
    terrain.extend(names)
    world = root / "native-pbr-flat.wrp"
    world.write_bytes(terrain)
    mission = root / "native-pbr.abel"
    mission.mkdir(exist_ok=True)
    (mission / "mission.sqm").write_text('''version=11;
class Mission {
 randomSeed=42;
 class Intel {year=1985;month=6;day=21;hour=16;minute=0;};
 class Groups {items=1;class Item0 {side="WEST";class Vehicles {items=1;class Item0 {
  position[]={6400,50,6400};id=0;side="WEST";vehicle="SoldierWB";
  player="PLAYER COMMANDER";leader=1;skill=1;
  init="this allowDamage false;0 setRain 0;0 setOvercast 0;0 setFog 0";
 };};};};
};
class Intro {randomSeed=1;};class OutroWin {randomSeed=1;};class OutroLoose {randomSeed=1;};
''', encoding="ascii")
    result = dict(model=str(model), world=str(world), mission=str(mission),
                  materials=[dict(name=n, roughness=r, metalness=m) for n, r, m in arms],
                  bcr=[155, 95, 45, 160], nmo=[128, 128, 230, 255],
                  faces=len(faces), vertices=len(points), provenance="synthetic owned fixture")
    (root / "fixture.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(json.dumps(result))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    create_fixture(parser.parse_args().output.resolve())
