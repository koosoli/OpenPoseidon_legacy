#!/usr/bin/env python3
"""Generate the P3DM MLOD test fixtures (AST-011A).

Authored rather than copied. The corpora AST-007 indexed are licensed Bohemia
data that cannot be redistributed in this repository, so these carry the same
container and LOD *structure* as a real Arma model -- `MLOD 1.1` header, `P3DM`
LOD signature, variable-length texture/material strings per face, the P3DM TAGG
encoding, `#UVSet#` blocks and real LOD resolutions -- with geometry invented here.

Regenerate:  python tests/fixtures/mlod/make_p3dm_fixtures.py
"""
import pathlib
import struct

HERE = pathlib.Path(__file__).resolve().parent
BS = chr(92)  # written this way so no quoting layer between here and the file can eat it


def z(text: str) -> bytes:
    return text.encode("latin1") + b"\0"


def p3dm_lod(points, faces, uv_sets, resolution, properties=()):
    """One P3DM LOD, laid out exactly as the format stores it."""
    data = b"P3DM" + struct.pack("<IIIIII", 28, 256, len(points), len(points), len(faces), 0)
    for x, y, zz in points:
        data += struct.pack("<fffI", x, y, zz, 0)
    for _ in points:
        data += struct.pack("<fff", 0.0, 1.0, 0.0)
    for indices, texture, material in faces:
        data += struct.pack("<I", len(indices))
        # Four vertex slots are always stored, even for a triangle.
        slots = list(indices) + [0] * (4 - len(indices))
        for slot, vertex in enumerate(slots):
            data += struct.pack("<IIff", vertex, vertex, float(slot & 1), float(slot >> 1))
        data += struct.pack("<I", 0) + z(texture) + z(material)

    data += b"TAGG"
    for key, value in properties:
        payload = key.encode("latin1").ljust(64, b"\0") + value.encode("latin1").ljust(64, b"\0")
        data += bytes([1]) + z("#Property#") + struct.pack("<I", len(payload)) + payload
    for _ in range(uv_sets):
        payload = struct.pack("<I", 0) + b"\0" * (len(faces) * 4 * 8)
        data += bytes([1]) + z("#UVSet#") + struct.pack("<I", len(payload)) + payload
    data += bytes([1]) + z("#EndOfFile#") + struct.pack("<I", 0)
    return data + struct.pack("<f", resolution)


def main() -> None:
    texture = BS.join(["ca", "test", "data", "slab_co.tga"])
    material = BS.join(["ca", "test", "data", "slab.rvmat"])
    points = [(0.0, 0.0, 0.0), (1.0, 0.0, 0.0), (1.0, 1.0, 0.0), (0.0, 1.0, 0.0), (0.0, 0.0, 1.0)]

    # Two UV sets and a mixed triangle/quad LOD, because both are the common case
    # in the indexed corpus rather than an edge case worth a separate fixture.
    visual = p3dm_lod(
        points,
        [((0, 1, 2), texture, material), ((0, 2, 3, 4), "", material)],
        uv_sets=2,
        resolution=1.0,
        properties=[("lodnoshadow", "1")],
    )
    geometry = p3dm_lod(points[:4], [((0, 1, 2), "", "")], uv_sets=1, resolution=1.0e13)

    two_lod = b"MLOD" + struct.pack("<II", 257, 2) + visual + geometry
    (HERE / "p3dm_two_lod.p3d").write_bytes(two_lod)

    # A container whose LODs disagree, so per-LOD dispatch is tested rather than a
    # single decision made once for the file. The SP3X LOD is truncated on purpose:
    # this fixture exists to prove routing, and P3DM is rejected before the SP3X
    # reader is ever reached.
    mixed = b"MLOD" + struct.pack("<II", 257, 2) + visual + b"SP3X" + b"\0" * 64
    (HERE / "p3dm_mixed_signatures.p3d").write_bytes(mixed)

    unknown = b"MLOD" + struct.pack("<II", 257, 1) + b"XXXX" + b"\0" * 64
    (HERE / "unknown_lod_signature.p3d").write_bytes(unknown)

    for name in ("p3dm_two_lod.p3d", "p3dm_mixed_signatures.p3d", "unknown_lod_signature.p3d"):
        print(f"wrote {name}: {(HERE / name).stat().st_size} bytes")


if __name__ == "__main__":
    main()
