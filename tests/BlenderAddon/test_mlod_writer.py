"""The engine's MLOD and PAA writers through the DLL (p3d_lib.MLODWriter, write_paa): what is written reads back."""

import struct


def _cube():
    pts = [(x, y, z) for x in (-0.5, 0.5) for y in (0.0, 1.0) for z in (-0.5, 0.5)]
    quads = [(0, 1, 3, 2), (4, 6, 7, 5), (0, 4, 5, 1), (2, 3, 7, 6), (0, 2, 6, 4), (1, 5, 7, 3)]
    return pts, quads


def test_mlod_round_trip(p3d, tmp_path):
    pts, quads = _cube()
    w = p3d.MLODWriter()
    lod = w.add_lod(1.0, [*pts, (0, 2, 0), (0, 2, 1), (0, 4, 0)])  # + a proxy triangle: position, direction, up
    faces = [{"points": q, "uvs": [(0, 0), (1, 0), (1, 1), (0, 1)], "texture": "mymod\\data\\box.paa"} for q in quads]
    faces.append({"points": (8, 9, 10), "uvs": [(0, 0)] * 3})
    w.set_faces(lod, faces)
    w.add_selection(lod, "lid", [1, 3, 5, 7], [1, 1, 1, p3d.o2_weight(0.5)], [5])
    w.add_selection(lod, "proxy:\\mymod\\flag.01", [8, 9, 10], None, [6])
    w.add_property(lod, "class", "house")
    geo = w.add_lod(1e13, pts)
    w.set_faces(geo, [{"points": q, "uvs": [(0, 0)] * 4} for q in quads])
    w.add_selection(geo, "Component01", list(range(8)), None, list(range(6)))
    w.set_mass(geo, [10.0] * 8)
    out = str(tmp_path / "box.p3d")
    w.write(out)
    w.close()

    with open(out, "rb") as f:
        assert f.read(4) == b"MLOD"
    m = p3d.P3DModel(out)
    assert m.format == "MLOD"
    assert m.lod_count == 2
    assert m.lod_resolution(0) == 1.0
    assert abs(m.lod_resolution(1) - 1e13) < 1e13 * 1e-6  # a 32-bit float on the wire
    assert m.face_count(0) == 6 * 2 + 1  # quads come back as two triangles each
    assert [m.material_texture(0, i) for i in range(m.material_count(0))].count("mymod\\data\\box.paa") == 1
    names = [m.selection_name(0, i) for i in range(m.selection_count(0))]
    assert "lid" in names
    assert m.proxy_count(0) == 1
    assert "flag" in m.proxy_path(0, 0)
    props = {m.property_name(0, i): m.property_value(0, i) for i in range(m.property_count(0))}
    assert props.get("class") == "house"


def test_mlod_refuses_bad_faces(p3d, tmp_path):
    w = p3d.MLODWriter()
    lod = w.add_lod(1.0, [(0, 0, 0), (1, 0, 0), (0, 1, 0)])
    w.set_faces(lod, [{"points": (0, 1, 7), "uvs": [(0, 0)] * 3}])  # a point that is not there
    try:
        w.write(str(tmp_path / "bad.p3d"))
    except RuntimeError as e:
        assert "pf_mlod_write" in str(e)
    else:
        raise AssertionError("a face on a missing point was written")


def test_paa_round_trip(p3d, tmp_path):
    w = h = 32
    rgba = bytearray()
    for y in range(h):
        for x in range(w):
            rgba += struct.pack("4B", x * 8, y * 8, 128, 255)
    out = str(tmp_path / "t.paa")
    p3d.write_paa(rgba, w, h, out)
    img = p3d.PAAImage(out)
    assert (img.width, img.height) == (w, h)
    back = bytes(img.get_rgba())
    img.close()
    # DXT is lossy (5-6 bits a channel, a 4x4 block's two end colours): the corners near what went in, the right
    # way up
    for x, y in ((0, 0), (w - 1, 0), (0, h - 1)):
        r, g = back[(y * w + x) * 4], back[(y * w + x) * 4 + 1]
        assert abs(r - x * 8) <= 24 and abs(g - y * 8) <= 24
