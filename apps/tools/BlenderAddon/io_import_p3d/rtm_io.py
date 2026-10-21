"""RTM animation files (RTM_0100 / RTM_0101): read and write, as engine/Poseidon/Asset/Formats/RTM/RTMReader.hpp reads:

    char[8] magic; float step[1 or 3]; int32 phases; int32 bones; char[32] name[bones];
    per phase: float time; per bone: char[32] name, float[12] matrix

A bone's matrix (the 3x3 by columns -- aside, up, dir -- then the translation) takes a point of the soldier's rest
model (the MC skeleton's own space: x forward, y up, z left) to where the posed bone puts it in the game's model
space (x left, y up, forward -z); the engine applies it to the vertices as it is. Plain Python + numpy, so it runs
inside Blender and outside it (scripts/swim/ uses it through a shim).
"""

import struct

import numpy as np

NAME_LEN = 32


def read_rtm(path):
    with open(path, "rb") as f:
        return read_rtm_bytes(f.read(), path)


def read_rtm_bytes(data, name="<memory>"):
    magic = data[:8]
    at = 8
    if magic == b"RTM_0101":
        step = struct.unpack_from("<3f", data, at)
        at += 12
    elif magic == b"RTM_0100":
        step = (0.0, 0.0, struct.unpack_from("<f", data, at)[0])
        at += 4
    else:
        raise ValueError(f"{name}: not an RTM ({magic!r})")
    n_phases, n_bones = struct.unpack_from("<2i", data, at)
    at += 8
    names = []
    for _ in range(n_bones):
        names.append(data[at : at + NAME_LEN].split(b"\0", 1)[0].decode("latin-1").lower())
        at += NAME_LEN
    phases = []
    for _ in range(n_phases):
        t = struct.unpack_from("<f", data, at)[0]
        at += 4
        mats = []
        for _ in range(n_bones):
            at += NAME_LEN
            mats.append(np.array(struct.unpack_from("<12f", data, at)))
            at += 48
        phases.append((t, mats))
    return {"step": tuple(step), "names": names, "phases": phases}


def write_rtm(path, step, names, phases):
    """phases: [(time 0..1, [12 floats per bone in names order]), ...]; always written as RTM_0101."""
    out = bytearray(b"RTM_0101")
    out += struct.pack("<3f", *step)
    out += struct.pack("<2i", len(phases), len(names))
    enc = [n.encode("latin-1")[:NAME_LEN].ljust(NAME_LEN, b"\0") for n in names]
    for e in enc:
        out += e
    for t, mats in phases:
        out += struct.pack("<f", float(t))
        for e, m in zip(enc, mats):
            out += e
            out += struct.pack("<12f", *[float(v) for v in m])
    with open(path, "wb") as f:
        f.write(out)


def mat4(m):
    """12 stored floats (aside, up, dir, pos) -> 4x4."""
    M = np.eye(4)
    M[:3, 0], M[:3, 1], M[:3, 2], M[:3, 3] = m[0:3], m[3:6], m[6:9], m[9:12]
    return M


def unmat(M):
    return np.concatenate([M[:3, 0], M[:3, 1], M[:3, 2], M[:3, 3]])
