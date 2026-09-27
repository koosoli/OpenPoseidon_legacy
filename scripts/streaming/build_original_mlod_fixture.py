"""Explicit offline fixture input only; never executes Tools or the game.

Writes an original P3DM MLOD matching OriginalGrid in
test_geometry_page_mlod_producer.cpp. It does not substitute a synthetic
selected-geometry cache or claim retail content. Root owns actual execution.
"""

import argparse
import hashlib
import json
from pathlib import Path
import struct


def f32(value: float) -> float:
    return struct.unpack("<f", struct.pack("<f", value))[0]


def original_grid() -> bytes:
    data = bytearray()

    def u32(value: int) -> None:
        data.extend(struct.pack("<I", value))

    def scalar(value: float) -> None:
        data.extend(struct.pack("<f", value))

    def text(value: str) -> None:
        data.extend(value.encode("ascii"))

    def asciiz(value: str) -> None:
        text(value)
        data.append(0)

    def fixed(value: str) -> None:
        encoded = value.encode("ascii")
        if len(encoded) >= 64:
            raise ValueError("fixed source field exceeds canonical subset")
        data.extend(encoded + b"\0" * (64 - len(encoded)))

    def tag(name: str, size: int) -> None:
        data.append(1)
        asciiz(name)
        u32(size)

    text("MLOD")
    u32(0x101)  # major=1, minor=1, padding=0
    u32(2)
    for level, subdivisions in enumerate((1, 16)):
        n = subdivisions
        text("P3DM")
        for value in (28, 256, (n + 1) * (n + 1), 1, n * n, 0):
            u32(value)
        for z in range(n + 1):
            for x in range(n + 1):
                # Inputs are dyadic here. Explicit float32 products reproduce
                # the ordinary C++ float expression, without Python's float64
                # retaining extra precision in the curved elevation.
                px = f32(f32(f32(float(x) / n) * 4) - 2)
                pz = f32(f32(f32(float(z) / n) * 4) - 2)
                ax = f32(1 - f32(f32(px * px) / 4))
                az = f32(1 - f32(f32(pz * pz) / 4))
                height = f32(f32(f32(0.9) * ax) * az)
                scalar(px)
                scalar(height)
                scalar(pz)
                u32(0)  # actual raw point flags
        # Existing P3DM loader reverses faces; MeshBuild negates normals.
        # Author the fixture so its actual packed triangles and normals face up.
        for normal in (0.0, -1.0, 0.0):
            scalar(normal)
        for z in range(n):
            for x in range(n):
                u32(4)
                for cx, cz in ((x, z), (x + 1, z), (x + 1, z + 1), (x, z + 1)):
                    u32(cz * (n + 1) + cx)
                    u32(0)  # normal index
                    scalar(f32(float(cx) / n))
                    scalar(f32(float(cz) / n))
                u32(0)  # actual raw face flags
                asciiz("")  # no primary texture
                asciiz("")  # no RVMAT
        text("TAGG")
        tag("#Property#", 128)
        fixed("autocenter")
        fixed("0")
        tag("#EndOfFile#", 0)
        scalar(1.0 if level else 10.0)
    # Full original source size, independent of later CLOD cut/codec lengths.
    expected = 12 + sum(
        28 + (n + 1) ** 2 * 16 + 12 + n * n * 74 + 4
        + (1 + len("#Property#") + 1 + 4 + 128)
        + (1 + len("#EndOfFile#") + 1 + 4) + 4
        for n in (1, 16)
    )
    if len(data) != expected or len(data) > 128 * 1024:
        raise ValueError("unexpected original P3DM byte layout/size")
    return bytes(data)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    directory = parser.parse_args().directory.resolve(strict=False)
    if not directory.parent.is_dir():
        raise ValueError("parent must already exist")
    # No parent creation, reuse, recursive removal, or writes into old artifacts.
    # A pre-existing file, directory, or symlink refuses mkdir atomically.
    directory.mkdir(exist_ok=False)
    original = original_grid()
    invalid = original + b"\0"  # actual disallowed trailing source byte
    reports = []
    for name, content in (("input.p3d", original), ("invalid-tail.p3d", invalid)):
        path = directory / name
        with path.open("xb") as output:
            if output.write(content) != len(content):
                raise OSError("short private fixture write")
        reports.append({
            "path": str(path),
            "bytes": len(content),
            "sha256": hashlib.sha256(content).hexdigest(),
        })
    print(json.dumps({
        "schemaVersion": 1,
        "scope": "authored-original-P3DM-input-only; no-bake/runtime/retail-proof",
        "directory": str(directory),
        "lods": [
            {"resolution": 10, "points": 4, "faces": 1, "triangles": 2},
            {"resolution": 1, "points": 289, "faces": 256, "triangles": 512},
        ],
        "input": reports[0],
        "invalidTail": {**reports[1], "expectedProducerStatus": "Invalid"},
    }, separators=(",", ":")))


if __name__ == "__main__":
    main()
