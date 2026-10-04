"""Sinkhole W5: collision for the Forest's models -- the MLOD geometry LOD (convex Component01.. pieces the engine
collides men and vehicles against) and the roadway LOD (the surfaces men walk on), from the game's own colliders
where it has them and from the lowest visual LOD where it does not. Used by forest_objects.py.

What the engine needs (checked against LODShape::InitConvexComponents and the Reforger collision converter,
engine/Poseidon/Asset/Formats/Enfusion/XobCollisionGeometry.cpp):
  * each component a closed CONVEX solid in a named selection ComponentNN (01, 02, ...), at least 4 faces;
  * the plane the engine builds from a face written (a, b, c) has the normal (b - a) x (c - a), and it must point
    INTO the component (Plane::Distance >= 0 inside);
  * a geometry LOD carries a #Mass# per point.
"""
import struct

import numpy as np

RES_GEOMETRY = 1e13
RES_ROADWAY = 3e15


# ---- convex hull (incremental; small point sets) -------------------------------------------------------------
def convex_hull(points, eps_rel=1e-7):
    """Outward-wound triangles over the hull of `points` (N x 3): (vertices M x 3, faces K x 3) or None when the
    set is flat or degenerate."""
    P = np.unique(np.round(np.asarray(points, np.float64), 5), axis=0)
    if len(P) < 4:
        return None
    scale = max(np.ptp(P, axis=0).max(), 1e-6)
    eps = eps_rel * scale * 10
    i0 = int(np.argmin(P[:, 0]))
    i1 = int(np.argmax(np.linalg.norm(P - P[i0], axis=1)))
    d = P[i1] - P[i0]
    i2 = int(np.argmax(np.linalg.norm(np.cross(P - P[i0], d), axis=1)))
    n = np.cross(P[i1] - P[i0], P[i2] - P[i0])
    if np.linalg.norm(n) < eps * scale:
        return None
    dist = (P - P[i0]) @ n
    i3 = int(np.argmax(np.abs(dist)))
    if abs(dist[i3]) < eps * scale * scale:
        return None
    inside = (P[i0] + P[i1] + P[i2] + P[i3]) / 4

    def oriented(a, b, c):
        nn = np.cross(P[b] - P[a], P[c] - P[a])
        return (a, b, c) if nn @ (P[a] - inside) > 0 else (a, c, b)

    faces = [oriented(i0, i1, i2), oriented(i0, i1, i3), oriented(i0, i2, i3), oriented(i1, i2, i3)]
    order = [i for i in np.random.default_rng(7).permutation(len(P)) if i not in (i0, i1, i2, i3)]
    for p in order:
        visible = []
        for f in faces:
            a, b, c = f
            nn = np.cross(P[b] - P[a], P[c] - P[a])
            ln = np.linalg.norm(nn)
            if ln > 0 and (nn / ln) @ (P[p] - P[a]) > eps:
                visible.append(f)
        if not visible:
            continue
        edges = {}
        for a, b, c in visible:
            for e in ((a, b), (b, c), (c, a)):
                edges[e] = edges.get(e, 0) + 1
        horizon = [e for e in edges if (e[1], e[0]) not in edges]
        vis = set(visible)
        faces = [f for f in faces if f not in vis] + [(a, b, p) for a, b in horizon]
    used = sorted({i for f in faces for i in f})
    remap = {old: new for new, old in enumerate(used)}
    return P[used], np.array([[remap[i] for i in f] for f in faces], np.int64)


def kmeans(points, k, iters=15, seed=3):
    rng = np.random.default_rng(seed)
    k = max(1, min(k, len(points)))
    centres = points[rng.choice(len(points), k, replace=False)]
    for _ in range(iters):
        label = np.argmin(((points[:, None, :] - centres[None]) ** 2).sum(-1), axis=1)
        centres = np.array([points[label == j].mean(0) if np.any(label == j) else centres[j] for j in range(k)])
    return label


def thin(points, cap, seed=5):
    """At most `cap` points: the extremes along the axes and the diagonals, then a random rest."""
    if len(points) <= cap:
        return points
    dirs = np.array([[1, 0, 0], [0, 1, 0], [0, 0, 1], [1, 1, 1], [1, 1, -1], [1, -1, 1], [-1, 1, 1]], float)
    keep = set()
    for d in dirs:
        s = points @ d
        keep.update((int(np.argmin(s)), int(np.argmax(s))))
    rest = np.random.default_rng(seed).choice(len(points), cap - len(keep), replace=False)
    return points[sorted(keep | set(int(i) for i in rest))]


def mesh_components(vertices, max_pieces, piece_size=3.0, convex=False):
    """Convex pieces covering a mesh: its hull when it is convex (or small), else the hulls of k clusters."""
    V = np.asarray(vertices, np.float64)
    k = 1 if convex else int(np.clip(np.ceil(np.ptp(V, axis=0).max() / piece_size), 1, max_pieces))
    labels = kmeans(V, k) if k > 1 else np.zeros(len(V), int)
    out = []
    for j in range(k):
        h = convex_hull(thin(V[labels == j], 160))
        if h is not None:
            out.append(h)
    return out


def capsule_points(radius, height, direction, centre, sides=8):
    """An octagonal prism around a Unity capsule (its full length, caps included)."""
    half = max(height / 2, radius)
    a = np.linspace(0, 2 * np.pi, sides, endpoint=False)
    ring = np.stack([np.cos(a) * radius, np.sin(a) * radius], axis=1)
    pts = []
    for s in (-half, half):
        for x, y in ring:
            p = [0.0, 0.0, 0.0]
            p[direction] = s
            p[(direction + 1) % 3] = x
            p[(direction + 2) % 3] = y
            pts.append(p)
    return np.array(pts) + np.asarray(centre)


def sphere_points(radius, centre):
    g = (1 + 5 ** 0.5) / 2
    ico = np.array([[-1, g, 0], [1, g, 0], [-1, -g, 0], [1, -g, 0], [0, -1, g], [0, 1, g], [0, -1, -g], [0, 1, -g],
                    [g, 0, -1], [g, 0, 1], [-g, 0, -1], [-g, 0, 1]], float)
    return ico / np.linalg.norm(ico[0]) * radius + np.asarray(centre)


def box_points(size, centre):
    s = np.asarray(size) / 2
    return np.array([[x, y, z] for x in (-s[0], s[0]) for y in (-s[1], s[1]) for z in (-s[2], s[2])]) + np.asarray(centre)


def transform(points, m):
    m = np.asarray(m)
    return points @ m[:3, :3].T + m[:3, 3]


# ---- the LODs as MLOD bytes ----------------------------------------------------------------------------------
def geometry_lod(pieces, total_mass, properties):
    """pieces: [(vertices, outward faces)]. Written inward (the engine's convention), one ComponentNN each."""
    pts, faces, sels = [], [], []
    for vs, fs in pieces:
        base = len(pts)
        centroid = vs.mean(0)
        sel_f = []
        for a, b, c in fs:
            # outward (a, b, c) -> write (a, c, b): (c - a) x (b - a) points inward
            if np.cross(vs[c] - vs[a], vs[b] - vs[a]) @ (centroid - vs[a]) < 0:
                b, c = c, b
            sel_f.append(len(faces))
            faces.append(((base + a, base + c, base + b), "", ""))
        sel_p = list(range(base, base + len(vs)))
        pts.extend(vs.tolist())
        sels.append((sel_p, sel_f))
    normals = []
    for (vs, fs) in pieces:
        c = vs.mean(0)
        for v in vs:
            nrm = v - c
            ln = np.linalg.norm(nrm)
            normals.append((nrm / ln).tolist() if ln > 1e-9 else [0.0, 1.0, 0.0])
    selections = {f"Component{i + 1:02d}": s for i, s in enumerate(sels)}
    mass = [total_mass / max(len(pts), 1)] * len(pts)
    return lod_bytes(pts, normals, faces, selections, mass, properties, RES_GEOMETRY)


def roadway_lod(vertices, triangles, min_up=0.35):
    """The faces a man may stand on: the mesh's upward-facing triangles (written in the visual LOD's order)."""
    V = np.asarray(vertices, np.float64)
    T = np.asarray(triangles, np.int64)
    n = np.cross(V[T[:, 1]] - V[T[:, 0]], V[T[:, 2]] - V[T[:, 0]])
    ln = np.linalg.norm(n, axis=1)
    keep = T[(ln > 1e-9) & (np.abs(n[:, 1]) / np.maximum(ln, 1e-12) > min_up)]
    if not len(keep):
        return None
    used = np.unique(keep)
    remap = {int(o): i for i, o in enumerate(used)}
    faces = [((remap[int(a)], remap[int(b)], remap[int(c)]), "", "") for a, b, c in keep]
    return lod_bytes(V[used].tolist(), [[0.0, 1.0, 0.0]] * len(used), faces, {}, None, [], RES_ROADWAY)


def lod_bytes(points, normals, faces, selections, mass, properties, resolution):
    """One P3DM 28/256 LOD (MLODStructures.hpp) from lists: faces [((p0, p1, p2), texture, material)]."""
    np_, nf = len(points), len(faces)
    out = [b"P3DM", struct.pack("<iiiiii", 28, 256, np_, len(normals), nf, 0)]
    out.append(b"".join(struct.pack("<fffi", *p, 0) for p in points))
    out.append(b"".join(struct.pack("<fff", *n) for n in normals))
    for corners, tex, mat in faces:
        rec = [struct.pack("<i", 3)]
        for i in range(4):
            p = corners[i] if i < 3 else 0
            rec.append(struct.pack("<iiff", p, p if i < 3 else 0, 0.0, 0.0))
        rec.append(struct.pack("<I", 0))
        rec.append(tex.encode() + b"\0" + mat.encode() + b"\0")
        out.append(b"".join(rec))
    out.append(b"TAGG")
    for name, (pts, fcs) in selections.items():
        pay = bytearray(np_ + nf)
        for p in pts:
            pay[p] = 1
        for f in fcs:
            pay[np_ + f] = 1
        out.append(b"\x01" + name.encode() + b"\0" + struct.pack("<i", len(pay)) + bytes(pay))
    if mass is not None:
        pay = struct.pack(f"<{np_}f", *mass)
        out.append(b"\x01#Mass#\0" + struct.pack("<i", len(pay)) + pay)
    for k, v in properties:
        pay = k.encode().ljust(64, b"\0") + v.encode().ljust(64, b"\0")
        out.append(b"\x01#Property#\0" + struct.pack("<i", len(pay)) + pay)
    out.append(b"\x01#EndOfFile#\0" + struct.pack("<i", 0))
    out.append(struct.pack("<f", resolution))
    return b"".join(out)
