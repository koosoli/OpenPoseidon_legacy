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


def placement_model(index, reference_all, registered_warming=False):
    # The first two placements share one physical model in the weak-registration
    # fixture; the remaining 256 placements reference every other inventory row.
    return max(0, index - 1) if registered_warming else index if reference_all else 0


def build_world(model_path=MODEL_PATH, model_count=1, placements=PLACEMENTS, reference_all=False, registered_warming=False):
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
    out += leaf + pack("i", len(placements) * 60)  # object offsets and byte count
    out += leaf + pack("i", 0)  # map-info offsets and byte count
    out += bulk(zero_grid) * 2  # persistent and subdivision hints
    out += pack("2i", max(p[0] for p in placements), LAND * LAND * 4)
    out += bytes(LAND * LAND * 4)  # every road-link count is zero
    for index, (identity, x, y, z) in enumerate(placements):
        out += pack("2i12fi", identity, placement_model(index, reference_all, registered_warming), 1, 0, 0, 0, 1, 0, 0, 0, 1, x, y, z, 0)
    return bytes(out)


def build_model(half_depth=6.0):
    source = Path(__file__).resolve().parents[1] / "showcase" / "build_showcase.py"
    spec = importlib.util.spec_from_file_location("original_showcase_fixture", source)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.model((0.5, 3.0, half_depth), "", "")


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


def verify_world(payload, model_path=MODEL_PATH, model_count=1, placements=PLACEMENTS, reference_all=False, registered_warming=False):
    r = Cursor(payload)
    assert r.take(4) == b"OPRW"
    assert r.unpack("6if") == (25, 0, LAND, LAND, LAND, LAND, CELL)
    assert r.take(10) == bytes(10)
    assert r.unpack("i") == (1,)
    assert r.unpack("3f") == (0.0, HEIGHT, 0.0)
    assert r.take(5) == bytes(5)
    assert r.grid(LAND * LAND) == bytes(LAND * LAND)
    assert r.grid(LAND * LAND) == bytes(LAND * LAND)
    assert struct.unpack(f"<{LAND * LAND}f", r.grid(LAND * LAND * 4)) == (HEIGHT,) * (LAND * LAND)
    assert r.unpack("i") == (1,) and r.z() == "" and r.take(1) == b"\0"
    assert r.unpack("i") == (model_count,)
    assert [r.z() for _ in range(model_count)] == model_paths(model_path, model_count)
    assert r.unpack("i") == (0,)
    assert r.take(5) == bytes(5) and r.unpack("i") == (len(placements) * 60,)
    assert r.take(5) == bytes(5) and r.unpack("i") == (0,)
    assert r.grid(LAND * LAND) == bytes(LAND * LAND) and r.grid(LAND * LAND) == bytes(LAND * LAND)
    assert r.unpack("2i") == (max(p[0] for p in placements), LAND * LAND * 4)
    assert r.take(LAND * LAND * 4) == bytes(LAND * LAND * 4)
    for index, (identity, x, y, z) in enumerate(placements):
        assert r.unpack("2i12fi") == (identity, placement_model(index, reference_all, registered_warming), 1, 0, 0, 0, 1, 0, 0, 0, 1, x, y, z, 0)
    assert r.pos == len(payload)


def verify_model(payload, half_depth=6.0):
    r = Cursor(payload)
    assert r.take(4) == b"MLOD" and r.unpack("2I") == (257, 4)
    for level, resolution in enumerate((1.0, 1e13, 6e15, 7e15)):
        assert r.take(4) == b"P3DM"
        assert r.unpack("6I") == (28, 256, 8, 6, 6, 0)
        vertices = [r.unpack("3fI") for _ in range(8)]
        assert {p[0] for p in vertices} == {-0.5, 0.5}
        assert {p[1] for p in vertices} == {0.0, 3.0}
        assert {p[2] for p in vertices} == {-half_depth, half_depth}
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


def generate(output, absolute_model_path=False, model_count=1, large_wall=False, reference_all=False, positive_warming=False, registered_warming=False):
    output = output.resolve()
    model_file = output / "streaming_fixture" / "wall.p3d"
    model_path = str(model_file) if absolute_model_path else MODEL_PATH
    # A 200 m origin offset exposes the legacy segment search's fixed 25 m
    # origin-cell padding. The original wall still intersects the same ray;
    # eight vertices keep this a correctness fixture rather than a load test.
    placements = tuple((p[0], p[1], p[2], 1400.0) for p in PLACEMENTS) if large_wall else PLACEMENTS
    if reference_all:
        if model_count != 257 or large_wall:
            raise ValueError("Referenced-model capacity fixture requires 257 models and ordinary walls")
        placements = tuple((1001 + i, 1200.0 + 30.0 * (i % 2), 11.5, 1200.0) for i in range(model_count))
    if positive_warming:
        if not reference_all or model_count != 257 or large_wall:
            raise ValueError("Positive warming fixture needs the complete 257-model capacity inventory")
        # Only two models enter either actor's camera window. The other 255
        # remain referenced metadata, outside both camera windows/actor regions.
        placements = PLACEMENTS + tuple((1001+i, 650.0, 11.5, 650.0) for i in range(2, model_count))
    if registered_warming:
        if not reference_all or model_count != 257 or large_wall or positive_warming:
            raise ValueError("Registered warming needs its separate complete 257-model fixture")
        if len(model_path) >= 128 or any(len(p) >= 128 for p in model_paths(model_path, model_count)):
            raise ValueError("Registered warming requires short canonical model paths; use a private temp directory")
        placements = PLACEMENTS + tuple((1003+i, 650.0, 11.5, 650.0) for i in range(256))
    half_depth = 206.0 if large_wall else 6.0
    world, model = build_world(model_path, model_count, placements, reference_all, registered_warming), build_model(half_depth)
    verify_world(world, model_path, model_count, placements, reference_all, registered_warming)
    verify_model(model, half_depth)
    # No implicit output location and no cleanup/deletion of caller files.
    model_file.parent.mkdir(parents=True, exist_ok=True)
    model_file.write_bytes(model)
    if reference_all:
        for i in range(1, model_count):
            model_file.with_name(model_file.stem + f"_unused_{i:03}.p3d").write_bytes(model)
    (output / "simulation-residency.wrp").write_bytes(world)
    metadata = {
        "schema": 1, "land_range": LAND, "terrain_range": LAND, "cell_metres": CELL,
        "elevation": HEIGHT, "models": model_paths(model_path, model_count), "absolute_model_path": absolute_model_path,
        "large_wall": large_wall, "reference_all_models": reference_all, "positive_warming": positive_warming, "registered_warming": registered_warming, "half_depth": half_depth, "placements": [
            {"id": p[0], "model_index": placement_model(i, reference_all, registered_warming), "position": list(p[1:])} for i, p in enumerate(placements)],
        "camera": [100, 80, 100], "actor": [1190, 10, 1200],
        "fire_segment": [[1190, 11.5, 1200], [1210, 11.5, 1200]],
        "wall_world_bounds": [[p[1] - 0.5, 10, p[3] - half_depth,
                               p[1] + 0.5, 13, p[3] + half_depth] for p in placements],
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
    parser.add_argument("--land-side", type=int, choices=(32, 256), default=32,
                        help="256 preserves the existing cargo mission coordinates on an original flat modern world")
    parser.add_argument("--absolute-model-path", action="store_true", help="Embed the private absolute loose model path; --addon-root accepts archives, not loose folders")
    parser.add_argument("--model-count", type=int, choices=(1, 257, 8193), default=1, help="Unused metadata rows or bounded referenced-model fixture")
    parser.add_argument("--reference-all-models", action="store_true", help="257 original walls reference every model to test the separate active-work capacity")
    parser.add_argument("--large-wall", action="store_true", help="Original wall intersects the ray with its origin 200 m away from it")
    parser.add_argument("--positive-warming", action="store_true", help="Separate two camera-loaded walls from the other 255 referenced models")
    parser.add_argument("--registered-warming", action="store_true", help="258 placements reference 257 models; the two query walls share model 0")
    args = parser.parse_args()
    LAND = args.land_side
    manifest = generate(args.output, args.absolute_model_path, args.model_count, args.large_wall, args.reference_all_models, args.positive_warming, args.registered_warming)
    print(json.dumps({"output": str(args.output.resolve()), "world_sha256": manifest["world_sha256"],
                      "model_sha256": manifest["model_sha256"]}, sort_keys=True))
