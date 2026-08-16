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


def p3dm_lod(points, faces, uv_sets, resolution, properties=(), selections=(), mass=None):
    """One P3DM LOD, laid out exactly as the format stores it.

    `selections` is a sequence of (name, point_indices, face_indices) named
    selections -- one weight byte per point then one flag byte per face, the
    layout readTAGGNamedSelection expects. `mass` is one float per point and is
    written as the `#Mass#` tagg (COL-001). Both default to absent, so the
    fixtures that predate them are byte-identical.
    """
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
    for name, point_indices, face_indices in selections:
        payload = bytes(1 if i in point_indices else 0 for i in range(len(points)))
        payload += bytes(1 if i in face_indices else 0 for i in range(len(faces)))
        data += bytes([1]) + z(name) + struct.pack("<I", len(payload)) + payload
    if mass is not None:
        payload = b"".join(struct.pack("<f", m) for m in mass)
        data += bytes([1]) + z("#Mass#") + struct.pack("<I", len(payload)) + payload
    for key, value in properties:
        payload = key.encode("latin1").ljust(64, b"\0") + value.encode("latin1").ljust(64, b"\0")
        data += bytes([1]) + z("#Property#") + struct.pack("<I", len(payload)) + payload
    # Payload is `uint32 id` then one (u,v) pair per ACTUAL face vertex -- face.n
    # entries, not the four slots a face record reserves. Measured across 3,598
    # UV-set blocks in the indexed Arma 2/OA corpus: every block was exactly
    # 4 + 8 * sum(face.n) bytes and none matched the four-slot reading.
    for set_index in range(uv_sets):
        payload = struct.pack("<I", set_index)
        for indices, _texture, _material in faces:
            for slot in range(len(indices)):
                # Channel 1 is deliberately offset from channel 0, so a reader that
                # collapses them produces visibly wrong numbers rather than
                # coincidentally right ones.
                payload += struct.pack("<ff", slot * 0.25 + set_index, set_index * 0.5)
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

    # Two coincident triangles differing only in their second UV channel. A reader
    # that merges vertices on uv0 alone collapses them to three vertices and loses
    # uv1 entirely -- the failure AST-011C exists to prevent.
    collapse_points = [(0.0, 0.0, 0.0), (1.0, 0.0, 0.0), (1.0, 1.0, 0.0)]
    collapse = b"P3DM" + struct.pack("<IIIIII", 28, 256, 3, 3, 2, 0)
    for x, y, zz in collapse_points:
        collapse += struct.pack("<fffI", x, y, zz, 0)
    for _ in collapse_points:
        collapse += struct.pack("<fff", 0.0, 1.0, 0.0)
    for _ in range(2):  # identical faces: same points, same normals, same uv0
        collapse += struct.pack("<I", 3)
        for vertex in [0, 1, 2, 0]:
            collapse += struct.pack("<IIff", vertex, vertex, 0.0, 0.0)
        collapse += struct.pack("<I", 0) + z("") + z("")
    collapse += b"TAGG"
    for set_index in range(2):
        payload = struct.pack("<I", set_index)
        for face_index in range(2):
            for _slot in range(3):
                # Channel 0 uniform; channel 1 separates the two faces.
                value = 0.0 if set_index == 0 else float(face_index)
                payload += struct.pack("<ff", value, value)
        collapse += bytes([1]) + z("#UVSet#") + struct.pack("<I", len(payload)) + payload
    collapse += bytes([1]) + z("#EndOfFile#") + struct.pack("<I", 0) + struct.pack("<f", 1.0)
    (HERE / "p3dm_uv_collapse.p3d").write_bytes(b"MLOD" + struct.pack("<II", 257, 1) + collapse)

    # A UV block that is internally consistent -- its declared size matches its own
    # payload, so the stream stays in sync -- but whose length does not correspond
    # to the face-vertex total. The reader must skip it rather than decode a short
    # buffer into plausible-looking wrong UVs.
    short = b"P3DM" + struct.pack("<IIIIII", 28, 256, 3, 3, 1, 0)
    for x, y, zz in collapse_points:
        short += struct.pack("<fffI", x, y, zz, 0)
    for _ in collapse_points:
        short += struct.pack("<fff", 0.0, 1.0, 0.0)
    short += struct.pack("<I", 3)
    for vertex in [0, 1, 2, 0]:
        short += struct.pack("<IIff", vertex, vertex, 0.0, 0.0)
    short += struct.pack("<I", 0) + z("") + z("")
    short += b"TAGG"
    good = struct.pack("<I", 0) + struct.pack("<ff", 0.0, 0.0) * 3   # correct: 3 face vertices
    bad = struct.pack("<I", 1) + struct.pack("<ff", 0.0, 0.0) * 2    # one pair short
    for payload in (good, bad):
        short += bytes([1]) + z("#UVSet#") + struct.pack("<I", len(payload)) + payload
    short += bytes([1]) + z("#EndOfFile#") + struct.pack("<I", 0) + struct.pack("<f", 1.0)
    (HERE / "p3dm_uv_bad_size.p3d").write_bytes(b"MLOD" + struct.pack("<II", 257, 1) + short)

    unknown = b"MLOD" + struct.pack("<II", 257, 1) + b"XXXX" + b"\0" * 64
    (HERE / "unknown_lod_signature.p3d").write_bytes(unknown)

    # COL-001: a model with a collidable geometry LOD -- a 2 x 3 x 4 m box of six
    # QUADS in one `Component01` selection, carrying a `#Mass#` tagg (8 x 150 kg).
    # Quads on purpose: the selection encoding that numbered a triangulated
    # stream lost every quad, and a box component is nothing but quads. The
    # winding is the engine's inward-normal convention after the loader's index
    # reversal, so ConvexComponent::IsInside holds for the box centre.
    box = [(0.0, 0.0, 0.0), (2.0, 0.0, 0.0), (2.0, 0.0, 4.0), (0.0, 0.0, 4.0),
           (0.0, 3.0, 0.0), (2.0, 3.0, 0.0), (2.0, 3.0, 4.0), (0.0, 3.0, 4.0)]
    box_quads = [(0, 1, 2, 3), (4, 7, 6, 5), (0, 4, 5, 1), (1, 5, 6, 2), (2, 6, 7, 3), (3, 7, 4, 0)]
    # The loader swaps slots 0<->1 and 2<->3 of every face on load, so the file
    # stores the pre-swap order of the sequence the engine should end up with.
    stored = [(q[1], q[0], q[3], q[2]) for q in box_quads]
    geometry_box = p3dm_lod(
        box,
        [(q, "", "") for q in stored],
        uv_sets=0,
        resolution=1.0e13,
        selections=[("Component01", set(range(8)), set(range(6)))],
        mass=[150.0] * 8,
    )
    geometry_mass = b"MLOD" + struct.pack("<II", 257, 2) + visual + geometry_box
    (HERE / "p3dm_geometry_mass.p3d").write_bytes(geometry_mass)

    for name in ("p3dm_two_lod.p3d", "p3dm_mixed_signatures.p3d", "p3dm_uv_collapse.p3d",
                 "p3dm_uv_bad_size.p3d", "unknown_lod_signature.p3d", "p3dm_geometry_mass.p3d"):
        print(f"wrote {name}: {(HERE / name).stat().st_size} bytes")


if __name__ == "__main__":
    main()
