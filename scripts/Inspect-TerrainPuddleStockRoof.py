"""Read the first visual LOD of a stock ODOL-7 shelter; never modify the asset.

Wire layout follows P3DStructures.hpp and SSCompress::Decode. This deliberately
does not pretend to inspect every LOD, object transform or GPU admission.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import struct


class Reader:
    def __init__(self, data):
        self.data, self.at = data, 0

    def take(self, count):
        if count < 0 or self.at + count > len(self.data):
            raise ValueError("Truncated model")
        result = self.data[self.at:self.at + count]
        self.at += count
        return result

    def read(self, fmt):
        result = struct.unpack("<" + fmt, self.take(struct.calcsize("<" + fmt)))
        return result[0] if len(result) == 1 else result

    def compressed(self, width):
        size = self.read("I") * width
        if size > 16 * 1024 * 1024:
            raise ValueError("Audit compressed-array bound exceeded")
        if size < 1024:
            return self.take(size)
        ring, cursor, flags, output = bytearray(b" " * 4096), 4078, 0, bytearray()
        while len(output) < size:
            flags >>= 1
            if flags & 256 == 0:
                flags = self.read("B") | 0xff00
            if flags & 1:
                values = [self.read("B")]
            else:
                low, high = self.read("BB")
                distance = low | ((high & 0xf0) << 4)
                count = min((high & 15) + 3, size - len(output))
                # Preserve overlapping back-references by updating each byte.
                start = cursor - distance
                for i in range(count):
                    value = ring[(start + i) & 4095]
                    output.append(value)
                    ring[cursor] = value
                    cursor = (cursor + 1) & 4095
                continue
            for value in values:
                output.append(value)
                ring[cursor] = value
                cursor = (cursor + 1) & 4095
        if self.read("I") != sum(output) & 0xffffffff:
            raise ValueError("Stock compressed-array checksum mismatch")
        return output

    def array(self, fmt):
        count = self.read("I")
        if count > 100000:
            raise ValueError("Audit vertex-array bound exceeded")
        return [self.read(fmt) for _ in range(count)]

    def string(self):
        end = self.data.index(0, self.at, min(self.at + 4096, len(self.data)))
        result = self.take(end - self.at).decode("ascii")
        self.take(1)
        return result


def inspect(path):
    data = path.read_bytes()
    reader = Reader(data)
    if reader.take(4) != b"ODOL" or reader.read("I") != 7:
        raise ValueError("Audit supports stock ODOL revision 7 only")
    lods = reader.read("I")
    if not 1 <= lods <= 100:
        raise ValueError("Invalid LOD count")
    reader.compressed(4)  # clip flags
    reader.compressed(8)  # UV coordinates
    points = reader.array("fff")
    reader.array("fff")  # vertex normals
    bounds = reader.read("iiffffffffff")
    textures = [reader.string() for _ in range(reader.read("I"))]
    reader.compressed(2)
    reader.compressed(2)
    face_count, _ = reader.read("II")
    if face_count > 100000:
        raise ValueError("Audit face bound exceeded")
    triangles = []
    for _ in range(face_count):
        _, texture, count = reader.read("IhB")
        if count not in (3, 4):
            raise ValueError("Unexpected non-triangle/quad stock face")
        indices = [reader.read("H") for _ in range(count)]
        if any(index >= len(points) for index in indices):
            raise ValueError("Out-of-range stock face index")
        name = textures[texture] if 0 <= texture < len(textures) else "<none>"
        for i in range(1, count - 1):
            triangles.append(([points[index] for index in (indices[0], indices[i], indices[i + 1])], name))
    if not all(math.isfinite(value) for point in points for value in point):
        raise ValueError("Nonfinite stock positions")
    lower_bound = bounds[3]
    probes = []
    for z in (-1.0, 0.0, 1.0):
        for x in (-1.0, 0.0, 1.0):
            hits = []
            for (a, b, c), texture in triangles:
                det = (b[2] - c[2]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[2] - c[2])
                if abs(det) < 1e-7:
                    continue
                u = ((b[2] - c[2]) * (x - c[0]) + (c[0] - b[0]) * (z - c[2])) / det
                v = ((c[2] - a[2]) * (x - c[0]) + (a[0] - c[0]) * (z - c[2])) / det
                if u >= -1e-6 and v >= -1e-6 and u + v <= 1 + 1e-6:
                    hits.append({"y": u * a[1] + v * b[1] + (1 - u - v) * c[1], "texture": texture})
            probes.append({"x": x, "z": z, "hits": sorted(hits, key=lambda row: row["y"])})
    # Conservative continuous-core check: report every low, roughly horizontal
    # triangle whose projected AABB intersects the 2x2 m central square. AABB
    # overlap can over-report a floor, but cannot hide a floor triangle there.
    lower_faces = []
    for (a, b, c), texture in triangles:
        ab = [b[i] - a[i] for i in range(3)]
        ac = [c[i] - a[i] for i in range(3)]
        normal = [ab[1]*ac[2]-ab[2]*ac[1], ab[2]*ac[0]-ab[0]*ac[2], ab[0]*ac[1]-ab[1]*ac[0]]
        length = math.sqrt(sum(value*value for value in normal))
        core_overlap = min(a[0],b[0],c[0]) <= 1 and max(a[0],b[0],c[0]) >= -1 and min(a[2],b[2],c[2]) <= 1 and max(a[2],b[2],c[2]) >= -1
        if length > 1e-7 and abs(normal[1])/length > 0.75 and core_overlap and max(a[1],b[1],c[1]) < lower_bound + 0.5:
            lower_faces.append({"texture": texture, "vertices": [a,b,c]})
    clearance = min((hit["y"] - lower_bound for probe in probes for hit in probe["hits"]), default=0)
    return {"source": str(path.resolve()), "sha256": hashlib.sha256(data).hexdigest(), "revision": 7,
            "scope": "first visual LOD only; central 2x2 m floor check, not runtime roof acceptance",
            "lods": lods, "points": len(points), "faces": face_count, "modelBounds": bounds[2:8],
            "probes": probes, "lowerHorizontalFacesInCore": lower_faces,
            "minimumCentralClearance": clearance,
            "centralFloorFreeRoofCandidate": not lower_faces and all(probe["hits"] for probe in probes) and clearance > 1.5}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("model", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    report = inspect(args.model)
    output = json.dumps(report, indent=2)
    if args.output:
        args.output.write_text(output + "\n", encoding="utf-8")
    else:
        print(output)
