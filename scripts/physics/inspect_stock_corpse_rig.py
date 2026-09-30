#!/usr/bin/env python3
"""Read-only ODOL7 stock corpse rig census; capsule outputs are envelopes, not a rig.

Requires NumPy. Reads bounded PBO members in memory; writes JSON only when --out
is supplied. No extraction, game startup, asset admission or runtime wiring.
Wire layout follows Asset/Formats/P3D/P3DStructures.hpp; SSCompress uses the
distance-based back-reference semantics of BISBinaryStream.hpp.
"""

import argparse
import collections
import hashlib
import json
import math
from pathlib import Path
import struct

import numpy as np

MAX_MEMBER = 4 * 1024 * 1024
MAX_ARRAY = 65536
MAX_ENTRIES = 20000
STOCK_BONES = tuple("pchodidlo lchodidlo pprsty lprsty lholen pholen pstehno lstehno "
                    "pzadek lzadek bricho zebra hrudnik krk prameno lrameno hlava "
                    "pbiceps lbiceps ploket lloket roura zbran pruka lruka".split())
ANATOMICAL = tuple(name for name in STOCK_BONES if name not in ("zbran", "roura"))
GROUPS = {
    "pelvis": ("pzadek", "lzadek", "bricho"),
    "chest": ("zebra", "hrudnik", "krk", "prameno", "lrameno"),
    "head": ("hlava",),
    "right_upper_arm": ("pbiceps",), "left_upper_arm": ("lbiceps",),
    "right_forearm_hand": ("ploket", "pruka"), "left_forearm_hand": ("lloket", "lruka"),
    "right_thigh": ("pstehno",), "left_thigh": ("lstehno",),
    "right_shin_foot": ("pholen", "pchodidlo", "pprsty"),
    "left_shin_foot": ("lholen", "lchodidlo", "lprsty"),
}
# Evidence candidates only. Overlap supplies a possible anchor, never a hinge axis.
JOINT_BOUNDARIES = ("bricho zebra", "krk hlava", "prameno pbiceps", "lrameno lbiceps",
                    "pbiceps ploket", "lbiceps lloket", "pzadek pstehno", "lzadek lstehno",
                    "pstehno pholen", "lstehno lholen")
ROLE_NAMES = ("memory", "geometry", "fireGeometry", "viewGeometry", "viewPilot",
              "viewGunner", "viewCommander", "viewCargo", "landContact", "roadway",
              "paths", "hitpoints")


class Refused(ValueError):
    """Malformed, unsupported or ambiguous input, with no partial admission."""


class Reader:
    def __init__(self, data):
        if len(data) > MAX_MEMBER:
            raise Refused("member exceeds byte cap")
        self.data, self.pos = data, 0

    def take(self, size):
        if size < 0 or self.pos + size > len(self.data):
            raise Refused("truncated member")
        result = self.data[self.pos:self.pos + size]
        self.pos += size
        return result

    def value(self, fmt):
        return struct.unpack("<" + fmt, self.take(struct.calcsize("<" + fmt)))

    def u32(self):
        return self.value("I")[0]

    def count(self):
        count = self.u32()
        if count > MAX_ARRAY:
            raise Refused("array count exceeds cap")
        return count

    def string(self):
        start = self.pos
        for _ in range(256):
            if self.take(1) == b"\0":
                return self.data[start:self.pos - 1].decode("ascii")
        raise Refused("unterminated or oversized string")

    def decompress(self, wanted):
        if not 0 < wanted <= MAX_MEMBER:
            raise Refused("decoded bytes exceed cap")
        ring = bytearray(b" " * 4096)
        cursor, flags, result = 4078, 0, bytearray()
        while len(result) < wanted:
            flags >>= 1
            if not flags & 256:
                flags = self.take(1)[0] | 0xff00
            if flags & 1:
                ch = self.take(1)[0]
                result.append(ch)
                ring[cursor] = ch
                cursor = (cursor + 1) & 4095
            else:
                low, high = self.take(2)
                distance = low | ((high & 0xf0) << 4)
                source = cursor - distance
                for index in range(min((high & 15) + 3, wanted - len(result))):
                    ch = ring[(source + index) & 4095]
                    result.append(ch)
                    ring[cursor] = ch
                    cursor = (cursor + 1) & 4095
        if self.u32() != sum(result) & 0xffffffff:
            raise Refused("SSCompress checksum mismatch")
        return bytes(result)

    def array(self, stride, compressed=False):
        size = self.count() * stride
        if size > MAX_MEMBER:
            raise Refused("array bytes exceed cap")
        return self.decompress(size) if compressed and size >= 1024 else self.take(size)

    def finish(self):
        if self.pos != len(self.data):
            raise Refused("unexpected trailing bytes")


def stream_string(stream):
    value = bytearray()
    for _ in range(256):
        ch = stream.read(1)
        if not ch:
            raise Refused("truncated archive string")
        if ch == b"\0":
            return value.decode("ascii")
        value += ch
    raise Refused("oversized archive string")


def archive_index(path):
    rows = []
    with path.open("rb") as stream:
        total_bytes = stream.seek(0, 2)
        stream.seek(0)
        for _ in range(MAX_ENTRIES):
            if stream.tell() > 8 * 1024 * 1024:
                raise Refused("archive table exceeds cap")
            name = stream_string(stream)
            encoded = stream.read(20)
            if len(encoded) != 20:
                raise Refused("truncated archive entry")
            magic, decoded, _, _, stored = struct.unpack("<5I", encoded)
            if not name and magic == 0x56657273:
                for _ in range(128):
                    if not stream_string(stream):
                        break
                    stream_string(stream)
                else:
                    raise Refused("archive properties exceed cap")
            elif not name and not magic and not stored:
                break
            else:
                rows.append(dict(name=name, magic=magic, decodedBytes=decoded, storedBytes=stored))
        else:
            raise Refused("archive entries exceed cap")
        offset = stream.tell()
        names = set()
        for row in rows:
            row["offset"] = offset
            offset += row["storedBytes"]
            name = row["name"].lower()
            if offset > total_bytes or name in names:
                raise Refused("invalid archive range or duplicate member")
            names.add(name)
    return rows, total_bytes


def read_member(stream, row):
    size = row["storedBytes"]
    decoded = row["decodedBytes"] or size
    if not 0 < size <= MAX_MEMBER or not 0 < decoded <= MAX_MEMBER:
        raise Refused("archive member exceeds cap")
    stream.seek(row["offset"])
    stored = stream.read(size)
    if len(stored) != size:
        raise Refused("truncated archive member")
    if row["magic"] == 0x43707273:
        reader = Reader(stored)
        payload = reader.decompress(decoded)
        reader.finish()
    elif row["magic"] == 0:
        if decoded != size:
            raise Refused("uncompressed member size mismatch")
        payload = stored
    else:
        raise Refused("unsupported PBO compression")
    return payload, hashlib.sha256(stored).hexdigest()


def selection_weights(selection, vertex_count):
    indices, weights = selection["indices"], selection["weights"]
    if weights and len(weights) != len(indices):
        raise Refused("selection weight count mismatch")
    if len(indices) != len(set(indices)) or any(i >= vertex_count or i < 0 for i in indices):
        raise Refused("duplicate or invalid selection vertex")
    # Shape::NamedSelection::Weight returns full membership for an absent array.
    return {index: weights[n] if weights else 255 for n, index in enumerate(indices)}


def parse_odol7(payload):
    reader = Reader(payload)
    if reader.take(4) != b"ODOL" or reader.u32() != 7:
        raise Refused("only exact ODOL7 is supported")
    lod_count = reader.count()
    if not 1 <= lod_count <= 100:
        raise Refused("LOD count exceeds cap")
    lods = []
    for _ in range(lod_count):
        reader.array(4, True)
        reader.array(8, True)
        points = np.array(list(struct.iter_unpack("<3f", reader.array(12))), dtype=float).reshape(-1, 3)
        normals = reader.array(12)
        if not np.isfinite(points).all() or not all(math.isfinite(v) for row in struct.iter_unpack("<3f", normals) for v in row):
            raise Refused("nonfinite vertex/normal")
        reader.take(48)  # LOD bounds
        for _ in range(reader.count()):
            reader.string()
        reader.array(2, True)
        reader.array(2, True)
        face_count = reader.count()
        reader.take(4)  # section offset
        for _ in range(face_count):
            _, _, corners = reader.value("IhB")
            if corners not in (3, 4) or any(i >= len(points) for i in reader.value("H" * corners)):
                raise Refused("invalid face corner/index")
        reader.take(reader.count() * 18)  # sections: IIihi
        selections = {}
        for _ in range(reader.count()):
            name = reader.string().lower()
            reader.array(2, True)
            reader.array(1, True)
            reader.array(4, True)
            reader.take(1)
            reader.array(4, True)
            indices = [i[0] for i in struct.iter_unpack("<H", reader.array(2, True))]
            weights = reader.array(1, True)
            if name in selections:
                raise Refused("duplicate named selection")
            selections[name] = selection_weights(dict(indices=indices, weights=weights), len(points))
        for _ in range(reader.count()):
            reader.string()
            reader.string()
        for _ in range(reader.count()):
            reader.take(4)
            reader.array(12)
        reader.take(12)
        proxy_count = reader.count()
        for _ in range(proxy_count):
            reader.string()
            reader.take(56)
        lods.append(dict(points=points, selections=selections, proxyCount=proxy_count))
    resolutions = reader.value("f" * lod_count)
    if not all(math.isfinite(v) for v in resolutions):
        raise Refused("nonfinite LOD resolution")
    reader.take(24)
    reader.take(12 + 12 + 24)  # aiming center; colors/view density; min/max
    bounding_center = reader.value("3f")
    reader.take(12 + 12 + 36)  # geometry center, center of mass, inverse inertia
    auto, lock, _, _, animation, _ = reader.value("5Bb")
    reader.array(4, True)
    reader.take(16)
    roles = reader.value("12b")
    reader.finish()
    if not all(math.isfinite(v) for v in bounding_center):
        raise Refused("nonfinite bounding center")
    if any(index < -1 or index >= lod_count for index in roles):
        raise Refused("invalid special LOD index")
    for lod, resolution in zip(lods, resolutions):
        lod["resolution"] = resolution
    return dict(lods=lods, boundingCenter=list(bounding_center), autoCenter=bool(auto),
                lockAutoCenter=bool(lock), allowAnimation=bool(animation), roles=dict(zip(ROLE_NAMES, roles)))


def fit_capsule(points, weights):
    """Weighted PCA axis with a conservative enclosing radius; no joint inference."""
    points, weights = np.asarray(points, dtype=float), np.asarray(weights, dtype=float)
    if points.ndim != 2 or points.shape[1] != 3 or len(points) != len(weights):
        raise Refused("invalid fit dimensions")
    if not np.isfinite(points).all() or not np.isfinite(weights).all() or (weights < 0).any():
        raise Refused("nonfinite or negative fit input")
    active = weights > 0
    points, weights = points[active], weights[active]
    if len(points) < 3:
        return dict(finiteEnvelope=False, reason="fewer than three positive-weight vertices")
    center = np.average(points, weights=weights, axis=0)
    offsets = points - center
    eigenvalues, eigenvectors = np.linalg.eigh((offsets * weights[:, None]).T @ offsets / weights.sum())
    axis = eigenvectors[:, -1]
    # Sign is deterministic even though the fit's endpoints have no anatomical labels.
    if axis[np.argmax(np.abs(axis))] < 0:
        axis = -axis
    along = offsets @ axis
    lower, upper = float(along.min()), float(along.max())
    radial = offsets - along[:, None] * axis
    radius = float(np.sqrt(np.square(radial).sum(axis=1)).max())
    if upper - lower <= 1e-6 or radius <= 1e-6:
        return dict(finiteEnvelope=False, reason="degenerate extent or zero-radius line")
    start, end = center + axis * lower, center + axis * upper
    return dict(finiteEnvelope=True, pointCount=len(points), start=start.tolist(), end=end.tolist(),
                radius=radius, segmentLength=upper - lower, covarianceEigenvalues=eigenvalues.tolist(),
                maximumRadialDistance=radius, kind="conservative_bind_space_envelope",
                jointFrameAdmitted=False)


def weighted_members(lod, names):
    merged = collections.defaultdict(int)
    for name in names:
        for index, weight in lod["selections"].get(name, {}).items():
            merged[index] += weight
    return {index: weight for index, weight in merged.items() if weight > 0}


def proxy_vertices(lod):
    return {index for name, members in lod["selections"].items() if name.startswith("proxy:")
            for index, weight in members.items() if weight > 0}


def envelope_for(lod, names):
    members = weighted_members(lod, names)
    # A flag proxy is itself weighted to hrudnik in stock models; its long
    # construction triangle is not the soldier's chest. Keep it in pose census,
    # but never fit a physical body to attachment helper geometry.
    excluded = proxy_vertices(lod) & members.keys()
    members = {index: weight for index, weight in members.items() if index not in excluded}
    if not members:
        return dict(finiteEnvelope=False, reason="no positive-weight vertices")
    indices = sorted(members)
    result = fit_capsule(lod["points"][indices], [members[index] for index in indices])
    result["excludedProxyVertices"] = len(excluded)
    return result


def lod_coverage(lod):
    memberships = [collections.Counter() for _ in lod["points"]]
    bone_counts = {}
    for bone in STOCK_BONES:
        members = lod["selections"].get(bone, {})
        bone_counts[bone] = sum(weight > 0 for weight in members.values())
        for index, weight in members.items():
            if weight > 0:
                memberships[index][bone] = weight
    influences = collections.Counter(len(item) for item in memberships)
    anatomical = sum(any(name in ANATOMICAL for name in item) for item in memberships)
    quantized_zero = sum(bool(item) and not any(weight * 128 // 255 > 0 for weight in item.values())
                         for item in memberships)
    return dict(vertices=len(memberships), anatomicalVertices=anatomical,
                stockBoneVertices=sum(bool(item) for item in memberships),
                unweightedVertices=influences.get(0, 0), proxySelectionVertices=len(proxy_vertices(lod)),
                sourceInfluenceHistogram=dict(sorted(influences.items())),
                overFourSourceInfluences=sum(count for number, count in influences.items() if number > 4),
                onlyQuantizedZeroInfluences=quantized_zero, bonePositiveVertexCounts=bone_counts)


def joint_candidates(lod):
    candidates = []
    for boundary in JOINT_BOUNDARIES:
        first, second = boundary.split()
        a, b = weighted_members(lod, (first,)), weighted_members(lod, (second,))
        overlap = sorted((a.keys() & b.keys()) - proxy_vertices(lod))
        candidate = dict(bones=[first, second], sharedPositiveVertices=len(overlap), jointFrameAdmitted=False)
        if overlap:
            weights = [min(a[index], b[index]) for index in overlap]
            candidate["overlapCentroid"] = np.average(lod["points"][overlap], weights=weights, axis=0).tolist()
            candidate["reason"] = "overlap anchor candidate only; anatomical hinge/cone axes unvalidated"
        else:
            candidate["reason"] = "no shared positive vertices; joint anchor and axes unresolved"
        candidates.append(candidate)
    return candidates


def inspect_model(payload):
    model = parse_odol7(payload)
    lods = model["lods"]
    graphical = [index for index, lod in enumerate(lods) if 0 <= lod["resolution"] < 1000]
    if not graphical:
        raise Refused("no graphical LOD")
    primary = min(graphical, key=lambda index: lods[index]["resolution"])
    lod = lods[primary]
    envelopes = {name: envelope_for(lod, names) for name, names in GROUPS.items()}
    bones = {name: envelope_for(lod, (name,)) for name in ANATOMICAL}
    missing = [name for name in ANATOMICAL if not weighted_members(lod, (name,))]
    return dict(revision=7, decodedBytes=len(payload), boundingCenter=model["boundingCenter"],
                autoCenter=model["autoCenter"], lockAutoCenter=model["lockAutoCenter"],
                allowAnimation=model["allowAnimation"], specialLodIndices=model["roles"],
                primaryGraphicalLod=primary, missingPrimaryAnatomicalSelections=missing,
                lods=[dict(index=index, resolution=item["resolution"], proxyCount=item["proxyCount"],
                           roles=[role for role, value in model["roles"].items() if value == index],
                           coverage=lod_coverage(item)) for index, item in enumerate(lods)],
                anatomicalEnvelopes=bones, groupedEnvelopes=envelopes,
                jointCandidates=joint_candidates(lod), finiteGroupedEnvelopes=sum(
                    envelope["finiteEnvelope"] for envelope in envelopes.values()),
                runtimeRigAdmitted=False, fallback="authored death animation",
                unresolved=["validated joint frames", "completed corpse pose seam", "physics ownership/lifetime"])


def census(archive, requested=None):
    rows, total_bytes = archive_index(archive)
    by_name = {row["name"].lower(): row for row in rows}
    names = requested or [row["name"] for row in rows
                          if row["name"].lower().startswith("mc ") and row["name"].lower().endswith(".p3d")]
    if not names or len(names) > 64:
        raise Refused("expected 1 to 64 bounded stock candidates")
    if len(names) != len({name.lower() for name in names}):
        raise Refused("duplicate requested candidate")
    results = []
    with archive.open("rb") as stream:
        for name in names:
            result = dict(member=name)
            try:
                row = by_name.get(name.lower())
                if row is None:
                    raise Refused("missing requested member")
                payload, stored_sha = read_member(stream, row)
                result.update(storedBytes=row["storedBytes"], decodedSha256=hashlib.sha256(payload).hexdigest(),
                              storedSha256=stored_sha, sourceCompression=hex(row["magic"]))
                result.update(inspect_model(payload), parsed=True)
            except (ValueError, UnicodeError, np.linalg.LinAlgError) as error:
                result.update(parsed=False, runtimeRigAdmitted=False, reason=str(error),
                              fallback="authored death animation")
            results.append(result)
    return dict(schemaVersion=1, sourceArchive=str(archive), archiveBytes=total_bytes,
                archiveEntries=len(rows), selection="all Data3D mc-space P3D members" if not requested else "explicit members",
                requestedCandidates=len(names), parsedCandidates=sum(result["parsed"] for result in results),
                runtimeRigsAdmitted=0, units="meters; raw ODOL7 model bind space, before engine recentering",
                capsuleScope="conservative positive-weight geometric envelopes excluding proxy:* helpers; no biomechanical admission",
                weightScope="raw positive source membership; runtime quantization/top-four/face rebinding not reproduced",
                models=results)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", type=Path, required=True)
    parser.add_argument("--member", action="append", help="repeat to restrict candidates; defaults to all mc-space P3Ds")
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()
    report = census(args.archive, args.member)
    encoded = json.dumps(report, indent=2, allow_nan=False) + "\n"
    if args.out:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(encoded, encoding="utf-8")
        print(json.dumps({key: report[key] for key in ("requestedCandidates", "parsedCandidates", "runtimeRigsAdmitted")}))
    else:
        print(encoded, end="")
    return int(report["parsedCandidates"] != report["requestedCandidates"])


if __name__ == "__main__":
    raise SystemExit(main())
