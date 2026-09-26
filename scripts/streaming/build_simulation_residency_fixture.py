"""Original, deterministic cold-simulation-residency fixture; Python stdlib only.

Reuses the original authored MLOD boxes from scripts/showcase/build_showcase.py.
WRP layout follows test_oprw25.cpp and PakCommand.cpp::Oprw::BuildOprw25.
No retail assets, launch, deletion, engine readiness override or asset whitelist.
The self-check validates the emitted subset; the engine reader remains the final
format/geometry authority and must be exercised separately by the runtime test.
"""

import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct


LAND = 32
CELL = 50.0
HEIGHT = 10.0
MODEL_PATH = r"streaming_fixture\wall.p3d"
PLACEMENTS = ((1001, 1200.0, 11.5, 1200.0), (1002, 1230.0, 11.5, 1200.0))


def pack(fmt, *values):
    return struct.pack("<" + fmt, *values)


def bulk(payload):
    """Engine LZO1X literal stream, selected solely by the 1024-byte threshold."""
    if len(payload) < 1024:
        return payload
    zeros, tail = divmod(len(payload) - 19, 255)
    return bytes(zeros + 1) + bytes((tail + 1,)) + payload + b"\x11\0\0"


def model_paths(model_path, model_count):
    # Unused inventory rows exercise refusal BEFORE geometry loading. Only the
    # first model is referenced by either placement; no missing asset is admitted.
    return [model_path] + [model_path[:-4] + f"_unused_{i:03}.p3d" for i in range(1, model_count)]


def build_world(model_path=MODEL_PATH, model_count=1):
    leaf = b"\0" * 5  # Uniform quadtree: root tag plus four-byte leaf.
    zero_grid = bytes(LAND * LAND)
    out = bytearray(b"OPRW" + pack("6if", 25, 0, LAND, LAND, LAND, LAND, CELL))
    out += leaf * 2  # geography, sound
    out += pack("i", 1) + pack("3f", 0, HEIGHT, 0)  # one mountain
    out += leaf  # terrain material index
    out += bulk(zero_grid) * 2  # grass approximation, primary texture index
    out += bulk(pack("f", HEIGHT) * (LAND * LAND))
    out += pack("i", 1) + b"\0\0"  # one empty fallback terrain material
    out += pack("i", model_count)
    for path in model_paths(model_path, model_count):
        out += path.encode("ascii") + b"\0"
    out += pack("i", 0)  # no config-backed static entity records
    out += leaf + pack("i", len(PLACEMENTS) * 60)  # object offsets and byte count
    out += leaf + pack("i", 0)  # map-info offsets and byte count
    out += bulk(zero_grid) * 2  # persistent and subdivision hints
    out += pack("2i", max(p[0] for p in PLACEMENTS), LAND * LAND * 4)
    out += bytes(LAND * LAND * 4)  # every road-link count is zero
    for identity, x, y, z in PLACEMENTS:
        out += pack("2i12fi", identity, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, x, y, z, 0)
    return bytes(out)


def build_model():
    source = Path(__file__).resolve().parents[1] / "showcase" / "build_showcase.py"
    spec = importlib.util.spec_from_file_location("original_showcase_fixture", source)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.model((0.5, 3.0, 6.0), "", "")


class Cursor:
    def __init__(self, data):
        self.data, self.pos = data, 0

    def take(self, count):
        end = self.pos + count
        if count < 0 or end > len(self.data):
            raise ValueError("Truncated fixture")
        result, self.pos = self.data[self.pos:end], end
        return result

    def unpack(self, fmt):
        return struct.unpack("<" + fmt, self.take(struct.calcsize("<" + fmt)))

    def z(self):
        end = self.data.index(0, self.pos)
        result = self.take(end - self.pos).decode("ascii")
        self.take(1)
        return result

    def grid(self, count):
        if count < 1024:
            return self.take(count)
        if self.take(1) != b"\0":
            raise ValueError("Expected a literal LZO stream")
        length = 18
        while True:
            byte = self.unpack("B")[0]
            if byte:
                length += byte
                break
            length += 255
        if length != count:
            raise ValueError("Wrong LZO literal length")
        result = self.take(count)
        if self.take(3) != b"\x11\0\0":
            raise ValueError("Missing LZO terminator")
        return result


def verify_world(payload, model_path=MODEL_PATH, model_count=1):
    r = Cursor(payload)
    assert r.take(4) == b"OPRW"
    assert r.unpack("6if") == (25, 0, LAND, LAND, LAND, LAND, CELL)
    assert r.take(10) == bytes(10)
    assert r.unpack("i") == (1,)
    assert r.unpack("3f") == (0.0, HEIGHT, 0.0)
    assert r.take(5) == bytes(5)
    assert r.grid(LAND * LAND) == bytes(LAND * LAND)
    assert r.grid(LAND * LAND) == bytes(LAND * LAND)
    assert struct.unpack("<1024f", r.grid(LAND * LAND * 4)) == (HEIGHT,) * 1024
    assert r.unpack("i") == (1,) and r.z() == "" and r.take(1) == b"\0"
    assert r.unpack("i") == (model_count,)
    assert [r.z() for _ in range(model_count)] == model_paths(model_path, model_count)
    assert r.unpack("i") == (0,)
    assert r.take(5) == bytes(5) and r.unpack("i") == (120,)
    assert r.take(5) == bytes(5) and r.unpack("i") == (0,)
    assert r.grid(1024) == bytes(1024) and r.grid(1024) == bytes(1024)
    assert r.unpack("2i") == (1002, 4096) and r.take(4096) == bytes(4096)
    for identity, x, y, z in PLACEMENTS:
        assert r.unpack("2i12fi") == (identity, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, x, y, z, 0)
    assert r.pos == len(payload)


def verify_model(payload):
    r = Cursor(payload)
    assert r.take(4) == b"MLOD" and r.unpack("2I") == (257, 4)
    for level, resolution in enumerate((1.0, 1e13, 6e15, 7e15)):
        assert r.take(4) == b"P3DM"
        assert r.unpack("6I") == (28, 256, 8, 6, 6, 0)
        vertices = [r.unpack("3fI") for _ in range(8)]
        assert {p[0] for p in vertices} == {-0.5, 0.5}
        assert {p[1] for p in vertices} == {0.0, 3.0}
        assert {p[2] for p in vertices} == {-6.0, 6.0}
        r.take(6 * 12)  # normals
        for _ in range(6):
            assert r.unpack("I") == (4,)
            for _ in range(4):
                vertex, normal, _, _ = r.unpack("IIff")
                assert vertex < 8 and normal < 6
            assert r.unpack("I") == (0,) and r.z() == "" and r.z() == ""
        assert r.take(4) == b"TAGG"
        tags = {}
        while True:
            assert r.take(1) == b"\1"
            name = r.z()
            tags[name] = r.take(r.unpack("I")[0])
            if name == "#EndOfFile#":
                break
        if level:
            assert tags["Component01"] == b"\1" * 14
            assert struct.unpack("<8f", tags["#Mass#"]) == (1000.0,) * 8
        assert r.take(4) == pack("f", resolution)
    assert r.pos == len(payload)


def generate(output, absolute_model_path=False, model_count=1):
    output = output.resolve()
    model_file = output / "streaming_fixture" / "wall.p3d"
    model_path = str(model_file) if absolute_model_path else MODEL_PATH
    world, model = build_world(model_path, model_count), build_model()
    verify_world(world, model_path, model_count)
    verify_model(model)
    # No implicit output location and no cleanup/deletion of caller files.
    model_file.parent.mkdir(parents=True, exist_ok=True)
    model_file.write_bytes(model)
    (output / "simulation-residency.wrp").write_bytes(world)
    metadata = {
        "schema": 1, "land_range": LAND, "terrain_range": LAND, "cell_metres": CELL,
        "elevation": HEIGHT, "models": model_paths(model_path, model_count), "absolute_model_path": absolute_model_path, "placements": [
            {"id": p[0], "model_index": 0, "position": list(p[1:])} for p in PLACEMENTS],
        "camera": [100, 80, 100], "actor": [1190, 10, 1200],
        "fire_segment": [[1190, 11.5, 1200], [1210, 11.5, 1200]],
        "wall_world_bounds": [[1199.5, 10, 1194, 1200.5, 13, 1206],
                              [1229.5, 10, 1194, 1230.5, 13, 1206]],
        "world_sha256": hashlib.sha256(world).hexdigest(),
        "model_sha256": hashlib.sha256(model).hexdigest(),
        "camera_isolation_env": {"WGR_OBJECT_STREAM_WINDOW_GROWTH": "0",
                                 "WGR_OBJECT_STREAM_RADIUS_CELLS": "8"},
        "notes": ["Absolute-model mode uses original loose files without an archive mount; relative mode needs a separately configured loose-file root.",
                  "Camera at 100 is isolated from walls at 1200/1230 with growth disabled.",
                  "No ResolveObject: actor demand must admit geometry with transient leases.",
                  "Generic inventory/classification/admission must establish readiness.",
                  "Self-check is not engine parsing, collision or runtime verification."]}
    (output / "fixture.json").write_text(json.dumps(metadata, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return metadata


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path, help="Private caller-selected output directory")
    parser.add_argument("--absolute-model-path", action="store_true", help="Embed the private absolute loose model path; --addon-root accepts archives, not loose folders")
    parser.add_argument("--model-count", type=int, choices=(1, 257), default=1, help="257 adds unused metadata rows for the service's pre-admission capacity refusal")
    args = parser.parse_args()
    manifest = generate(args.output, args.absolute_model_path, args.model_count)
    print(json.dumps({"output": str(args.output.resolve()), "world_sha256": manifest["world_sha256"],
                      "model_sha256": manifest["model_sha256"]}, sort_keys=True))
