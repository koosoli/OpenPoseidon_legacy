"""Original tiny ODOL7 source-diagnostic world; no retail data or game launch.

Uses the verified OPRW25 fixture writer. Nine bounded model identities allow the
runtime to exercise the eight-request diagnostic cap; only index0 has placements.
The triangles intentionally have no source class, animation, skeleton or proxies.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import build_simulation_residency_fixture as world


def original_odol7():
    data = bytearray(b"ODOL")
    def put(fmt, *values):
        data.extend(struct.pack("<" + fmt, *values))
    def point(x=0, y=0, z=0):
        put("3f", x, y, z)
    put("2I", 7, 4)
    for _ in range(4):
        put("4I", 3, 0, 0, 0)  # point flags
        put("I6f", 3, *([0] * 6))  # UVs
        put("I", 3)
        point(); point(1); point(0, 1)
        put("I", 3)
        for _ in range(3): point(0, 0, 1)
        put("2I", 0, 0)
        point(); point(1, 1); point(.5, .5); put("f", 1)
        put("3I", 0, 0, 0)
        put("3IH B 3H", 1, 0, 0, 0xffff, 3, 0, 1, 2)
        put("4I", 0, 0, 0, 0)  # sections, selections, properties, frames
        put("4I", 0, 0, 0, 0)
    put("4f", 1, 1e13, 7e15, 6e15)  # visual, physical, fire, view roles
    put("I2f3I", 0, 1, 1, 0, 0, 0)
    point(); put("2If", 0, 0, 0)
    point(); point(1, 1)
    for _ in range(3): point()
    put("9f", *([0] * 9))
    put("6BI", *([0] * 7))
    put("4f", 0, 0, 0, 0)
    put("12B", *([255] * 12))
    return bytes(data)


def generate(output):
    output = output.resolve()
    model = output / "op_src_original_probe.p3d"
    if len(str(model)) >= 128 or not str(model).isascii():
        raise ValueError("Caller must choose a short ASCII private fixture directory")
    paths = world.model_paths(str(model), 9)
    payload = original_odol7()
    wrp = world.build_world(str(model), 9)
    world.verify_world(wrp, str(model), 9)
    output.mkdir(parents=True, exist_ok=False)
    for path in paths:
        Path(path).write_bytes(payload)
    (output / "simulation-residency.wrp").write_bytes(wrp)
    metadata = {"schema": 1, "models": paths, "originalFormat": "ODOL7",
                "world_sha256": hashlib.sha256(wrp).hexdigest(),
                "model_sha256": hashlib.sha256(payload).hexdigest(),
                "notes": ["Original authored tiny triangles; engine parser is the format authority.",
                          "Only model index0 has placements; no simulation Ready override.",
                          "No source geometry is copied from retail content."]}
    (output / "fixture.json").write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
    return metadata


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    print(json.dumps(generate(parser.parse_args().output)))
