"""Original ODOL7 convex walls in a complete 257-model OPRW25 inventory.

No retail assets, class/Ready override, game launch or caller-file deletion.
The existing world writer validates WRP bytes; engine parsing and actual shape
adaptation are separate required checks, not certified by this generator.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import build_simulation_residency_fixture as world


def original_box():
    points = ((-.5,-1.5,-6),(.5,-1.5,-6),(.5,-1.5,6),(-.5,-1.5,6),
              (-.5,1.5,-6),(.5,1.5,-6),(.5,1.5,6),(-.5,1.5,6))
    quads = ((0,1,2,3),(4,7,6,5),(0,4,5,1),(1,5,6,2),(2,6,7,3),(3,7,4,0))
    data = bytearray(b"ODOL")
    def put(fmt, *values): data.extend(struct.pack("<"+fmt,*values))
    def point(p=(0,0,0)): put("3f",*p)
    def array(fmt, values):
        put("I",len(values))
        for value in values: put(fmt,value)
    put("2I",7,4)
    for lod in range(4):
        array("I",[0]*8)
        put("I16f",8,*([0]*16))
        put("I",8)
        for p in points: point(p)
        put("I",8)
        for _ in points: point((0,0,1))
        put("2I",0,0);point((-.5,-1.5,-6));point((.5,1.5,6));point();put("f",7)
        put("3I",0,0,0)  # textures, two edge arrays
        put("2I",6,0)
        for q in quads: put("IH B 4H",0,0xffff,4,*q)
        put("I",0)  # sections
        put("I",int(lod != 0))
        if lod:
            data.extend(b"Component01\0")
            array("H",range(6));array("B",[])  # source-face membership, no weights
            array("I",[]);put("B",0);array("I",[])
            array("H",range(8));array("B",[])  # ordinary static vertex membership
        put("2I",0,0)  # properties, frames
        put("4I",0xffffffff,0xffffffff,0,0)  # LOD colors/special, proxies
    put("4f",1,1e13,7e15,6e15)
    put("I2f3I",0,7,7,0,0,0);point();put("2If",0xffffffff,0xffffffff,0)
    point((-.5,-1.5,-6));point((.5,1.5,6))
    for _ in range(3): point()
    put("9f",*([0]*9));put("6B",*([0]*6))
    array("f",[1000]*8)
    put("4f",8000,1/8000,1,1)
    put("12b",-1,1,2,3,-1,-1,-1,-1,-1,-1,-1,-1)
    return bytes(data)


def generate(output):
    output = output.resolve()
    model = output / "op_cold_plain.p3d"
    paths = world.model_paths(str(model),257)
    if any(not path.isascii() or len(path.encode("ascii")) >=128 for path in paths):
        raise ValueError("Caller must choose a short ASCII private fixture path")
    placements = world.PLACEMENTS + tuple((1001+i,650,11.5,650) for i in range(2,257))
    payload = original_box()
    wrp = world.build_world(str(model),257,placements,True)
    world.verify_world(wrp,str(model),257,placements,True)
    output.mkdir(parents=True,exist_ok=False)
    for path in paths: Path(path).write_bytes(payload)
    (output / "simulation-residency.wrp").write_bytes(wrp)
    metadata = {"schema":1,"originalFormat":"ODOL7","models":paths,"modelCount":257,
                "actor":[1190,10,1200],"camera":[100,80,100],
                "nearPlacements":[{"id":p[0],"model_index":i,"position":list(p[1:])}
                                  for i,p in enumerate(world.PLACEMENTS)],
                "otherPlacementCount":255,"otherPlacementPosition":[650,11.5,650],
                "world_sha256":hashlib.sha256(wrp).hexdigest(),
                "model_sha256":hashlib.sha256(payload).hexdigest(),
                "notes":["Original six-face convex boxes, four explicit LOD roles, Component01 memberships with no authored animation weights.",
                         "Only two models are near the actor; all 257 are referenced by the world.",
                         "Source-format decode, live config, actual constructor and timing still require engine checks."]}
    (output / "fixture.json").write_text(json.dumps(metadata,indent=2)+"\n",encoding="utf-8")
    return metadata


if __name__ == "__main__":
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output",type=Path)
    print(json.dumps(generate(parser.parse_args().output)))
