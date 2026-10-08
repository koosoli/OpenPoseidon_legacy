"""Conservatively infer ordinary ground-road IDs in a prepared Worldgen output.

Offline only. This annotates an existing manifest; it neither regenerates assets
nor changes upstream validation, collision or elevation. OSM association is
geometric inference, not an exact generator provenance mapping.
"""
from __future__ import annotations

import argparse
from collections import Counter, defaultdict
from dataclasses import dataclass
from datetime import datetime, timezone
import hashlib
import json
import math
from pathlib import Path
import re
import struct

MAX_JSON = 64 * 1024 * 1024
MAX_OBJECTS = 131072
MAX_ELEMENTS = 100000
MAX_POINTS = 2000000
MAX_SEGMENTS = 200000
MAX_INDEX_REFS = 2000000
MAX_WRP = 32 * 1024 * 1024
RADIUS = 5.0
BUCKET = 64.0
ROAD = re.compile(r"^o/road/(sil|ces)(25|12|6)\.p3d$")
FALSE = frozenset(("", "no", "false", "0"))


def read_json(path: Path, maximum=MAX_JSON, identities=None):
    if not 0 < path.stat().st_size <= maximum:
        raise ValueError(f"JSON byte budget: {path}")
    payload = path.read_bytes()
    if not 0 < len(payload) <= maximum:
        raise ValueError(f"JSON changed/byte budget: {path}")
    if identities is not None:
        identities[path] = hashlib.sha256(payload).hexdigest()
    return json.loads(payload.decode("utf-8-sig"),
                      parse_constant=lambda value: (_ for _ in ()).throw(ValueError(f"Nonfinite JSON {value}")))


def encoded(value):
    return (json.dumps(value, indent=2, allow_nan=False) + "\n").encode("utf-8")


def digest(path):
    result = hashlib.sha256()
    total = 0
    with path.open("rb") as stream:
        while chunk := stream.read(1024*1024):
            total += len(chunk)
            if total > MAX_JSON:
                raise ValueError("Source changed/hash byte budget")
            result.update(chunk)
    return result.hexdigest()


def safe_tags(tags):
    for key in ("bridge", "tunnel", "covered", "indoor"):
        if str(tags.get(key, "")).strip().casefold() not in FALSE:
            return False
    for key in ("layer", "level"):
        try:
            value = float(tags.get(key, 0))
        except (TypeError, ValueError):
            return False
        if not math.isfinite(value) or value != 0:
            return False
    if str(tags.get("location", "")).casefold() not in ("", "surface", "ground"):
        return False
    if any(key.startswith(("bridge:", "tunnel:")) for key in tags):
        return False
    return tags.get("highway") not in ("construction", "proposed")


def bounds(manifest):
    world = manifest["world"]
    south, west, north, east = map(float, world["bbox"])
    cell = float(world["cell_size"])
    cells = world["cells"]
    if (not all(math.isfinite(v) for v in (south, west, north, east, cell)) or
            not -84 <= south < north <= 84 or not -180 <= west < east <= 180 or
            not isinstance(cells, int) or isinstance(cells, bool) or not 16 <= cells <= 2048 or
            not 0 < cell <= 1000 or north-south > .2 or east-west > .3):
        raise ValueError("Invalid/broad source geographic/grid contract")
    return south, west, north, east, cells*cell


def project(point, bbox):
    south, west, north, east, extent = bbox
    lat, lon = float(point["lat"]), float(point["lon"])
    if not math.isfinite(lat) or not math.isfinite(lon) or not -90 <= lat <= 90 or not -180 <= lon <= 180:
        raise ValueError("Nonfinite/invalid OSM coordinate")
    return (lon-west)/(east-west)*extent, (lat-south)/(north-south)*extent


@dataclass(frozen=True)
class Segment:
    way: int
    ordinal: int
    a: tuple[float, float]
    b: tuple[float, float]
    safe: bool


class Roads:
    def __init__(self, source, bbox):
        elements = source["elements"]
        if not isinstance(elements, list) or len(elements) > MAX_ELEMENTS:
            raise ValueError("OSM element budget")
        nodes = {e["id"]: e for e in elements if e.get("type") == "node" and "lat" in e and "lon" in e}
        self.segments = []
        self.grid = defaultdict(list)
        self.missing_geometry = 0
        refs = points = 0
        ids = set()
        extent = bbox[-1]
        for element in elements:
            tags = element.get("tags", {})
            if element.get("type") != "way" or "highway" not in tags:
                continue
            way = element["id"]
            if not isinstance(way, int) or isinstance(way, bool) or way in ids:
                raise ValueError("Invalid/duplicate highway way ID")
            ids.add(way)
            geometry = element.get("geometry")
            if geometry is None:
                geometry = [nodes.get(n) for n in element.get("nodes", [])]
            if not geometry or len(geometry) < 2 or any(p is None for p in geometry):
                self.missing_geometry += 1
                continue
            points += len(geometry)
            if points > MAX_POINTS:
                raise ValueError("OSM geometry point budget")
            projected = [project(point, bbox) for point in geometry]
            for ordinal, (a, b) in enumerate(zip(projected, projected[1:])):
                if math.dist(a, b) < 1e-6:
                    continue
                # Index only geometry that can affect a fully contained footprint.
                left, right = max(-RADIUS, min(a[0], b[0])-RADIUS), min(extent+RADIUS, max(a[0], b[0])+RADIUS)
                low, high = max(-RADIUS, min(a[1], b[1])-RADIUS), min(extent+RADIUS, max(a[1], b[1])+RADIUS)
                if left > right or low > high:
                    continue
                if len(self.segments) >= MAX_SEGMENTS:
                    raise ValueError("OSM segment budget")
                index = len(self.segments)
                self.segments.append(Segment(way, ordinal, a, b, safe_tags(tags)))
                for x in range(math.floor(left/BUCKET), math.floor(right/BUCKET)+1):
                    for z in range(math.floor(low/BUCKET), math.floor(high/BUCKET)+1):
                        refs += 1
                        if refs > MAX_INDEX_REFS:
                            raise ValueError("OSM spatial reference budget")
                        self.grid[(x, z)].append(index)
        self.ways = len(ids)

    def associate(self, point, direction):
        nearby = {}
        passages = defaultdict(list)
        for index in self.grid.get((math.floor(point[0]/BUCKET), math.floor(point[1]/BUCKET)), ()):
            segment = self.segments[index]
            dx, dz = segment.b[0]-segment.a[0], segment.b[1]-segment.a[1]
            squared = dx*dx+dz*dz
            t = max(0, min(1, ((point[0]-segment.a[0])*dx+(point[1]-segment.a[1])*dz)/squared))
            distance = math.hypot(point[0]-segment.a[0]-t*dx, point[1]-segment.a[1]-t*dz)
            if distance >= RADIUS:
                continue
            passages[segment.way].append(segment.ordinal)
            if segment.way not in nearby or distance < nearby[segment.way][0]:
                alignment = abs((direction[0]*dx+direction[1]*dz)/math.sqrt(squared))
                nearby[segment.way] = distance, alignment, segment.safe
        if not nearby:
            return None, "unassociated"
        if len(nearby) != 1:
            return None, "ambiguous_multiple_ways"
        way, (distance, alignment, safe) = next(iter(nearby.items()))
        if max(passages[way])-min(passages[way]) > 2:
            return None, "ambiguous_same_way_passages"
        if not safe:
            return None, "bridge_tunnel_layer_or_elevated_tags"
        if alignment < math.cos(math.radians(15)):
            return None, "direction_mismatch"
        return (way, distance), "accepted"


def road_length(model, matrix):
    match = ROAD.fullmatch(model.lower().replace("\\", "/"))
    if not match:
        return None
    if (not all(math.isfinite(v) for v in matrix) or abs(matrix[1]) > 1e-4 or abs(matrix[7]) > 1e-4 or
            abs(matrix[3]) > 1e-4 or abs(matrix[4]-1) > 1e-4 or abs(matrix[5]) > 1e-4 or
            abs(math.hypot(matrix[0], matrix[2])-1) > 1e-4 or
            abs(math.hypot(matrix[6], matrix[8])-1) > 1e-4 or
            abs(matrix[0]*matrix[6]+matrix[2]*matrix[8]) > 1e-4 or
            matrix[0]*matrix[8]-matrix[2]*matrix[6] <= 0):
        return None
    return {"25": 25.0, "12": 12.5, "6": 6.25}[match[2]]


def classify_record(model, matrix, roads, extent):
    length = road_length(model, matrix)
    if length is None:
        return None, "unsupported_model_or_transform"
    x, z = matrix[9], matrix[11]
    side = 4.55 if "sil" in model.casefold() else 1.75
    for sign in (-1, 1):
        for flank in (-1, 1):
            px = x+sign*length*.5*matrix[6]+flank*side*matrix[0]
            pz = z+sign*length*.5*matrix[8]+flank*side*matrix[2]
            if not 0 <= px <= extent or not 0 <= pz <= extent:
                return None, "footprint_outside_source_bounds"
    matches = []
    for offset in (0, -length*.5, length*.5):
        point = x+offset*matrix[6], z+offset*matrix[8]
        match, reason = roads.associate(point, (matrix[6], matrix[8]))
        if match is None:
            return None, reason
        matches.append(match)
    if len({way for way, _ in matches}) != 1:
        return None, "different_ways_across_footprint"
    return {"osmWay": matches[0][0], "maximumAssociationDistanceMetres": max(d for _, d in matches)}, "accepted"


def classify(manifest_path: Path, osm_path: Path | None = None):
    identities = {}
    manifest = read_json(manifest_path, 4*1024*1024, identities)
    bbox = bounds(manifest)
    name = manifest["world"]["name"]
    if not re.fullmatch(r"[a-z0-9_]{1,20}", name):
        raise ValueError("Unsafe world name")
    wrp = manifest_path.parent/"source"/name/(name+".wrp")
    if not wrp.is_file():
        wrp = manifest_path.parent/"source"/(name+".wrp")
    if not 0 < wrp.stat().st_size <= MAX_WRP:
        raise ValueError("WRP byte budget")
    osm_path = osm_path or manifest_path.parent/"osm-source.json"
    source = read_json(osm_path, identities=identities)
    roads = Roads(source, bbox)
    # A missing way may hide a crossing/bridge near an otherwise safe road.
    if roads.missing_geometry:
        raise ValueError("Highway geometry incomplete; cannot prove unique association")
    raw = wrp.read_bytes()
    if not 0 < len(raw) <= MAX_WRP:
        raise ValueError("WRP changed/byte budget")
    identities[wrp] = hashlib.sha256(raw).hexdigest()
    signature, width, height = struct.unpack_from("<4sii", raw)
    if signature != b"4WVR" or width != manifest["world"]["cells"] or height != width:
        raise ValueError("RVW4 dimensions/signature mismatch")
    offset = 12+width*height*4+512*32
    if len(raw) < offset+128 or (len(raw)-offset) % 128 or (len(raw)-offset)//128 > MAX_OBJECTS+1:
        raise ValueError("RVW4 object budget/length")
    selected = []
    counts = Counter()
    seen = set()
    terminated = False
    for position in range(offset, len(raw), 128):
        *matrix, object_id, model_bytes = struct.unpack_from("<12fI76s", raw, position)
        if model_bytes[0] == 0:
            terminated = position+128 == len(raw)
            break
        if b"\0" not in model_bytes or object_id > 2147483647 or object_id in seen:
            raise ValueError("Malformed/duplicate RVW4 record")
        seen.add(object_id)
        model = model_bytes.split(b"\0", 1)[0].decode("ascii")
        match, reason = classify_record(model, matrix, roads, bbox[-1])
        counts[reason] += 1
        if match:
            selected.append({"id": object_id, **match})
    if not terminated:
        raise ValueError("RVW4 missing final terminator")
    if any(digest(path) != identity for path, identity in identities.items()):
        raise ValueError("Source changed during classification; use immutable prepared input")
    manifest["earth_ground_road_ids"] = sorted(record["id"] for record in selected)
    receipt = {"schema": 1, "createdUTC": datetime.now(timezone.utc).isoformat(), "classification": "inferred_geometry_not_generator_provenance",
               "manifest": str(manifest_path), "manifestInputSHA256": identities[manifest_path], "wrp": str(wrp), "wrpSHA256": identities[wrp],
               "osm": str(osm_path), "osmSHA256": identities[osm_path], "bbox": manifest["world"]["bbox"], "records": len(seen),
               "selected": len(selected), "reasons": dict(counts), "highwayWays": roads.ways, "indexedSegments": len(roads.segments),
               "maximumAssociationDistanceMetresExclusive": RADIUS, "maximumTangentDifferenceDegrees": 15,
               "sourceValidationUnchanged": manifest.get("final_validation"), "associations": selected,
               "limits": "All three road-axis probes must associate uniquely to the same safe-tagged OSM way. Ambiguity preserves authored placement. Missing/wrong OSM tags remain a source limitation; this is not bridge or global road validation."}
    return manifest, receipt


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--osm", type=Path)
    parser.add_argument("--output", type=Path, help="Fresh annotated manifest beside its original; default manifest-earth-ground-roads.json")
    parser.add_argument("--apply", action="store_true", help="Explicitly annotate original manifest, after byte-identical backup")
    args = parser.parse_args()
    original = args.manifest.resolve(strict=True)
    if args.apply and args.output:
        parser.error("Choose --apply or --output")
    output = original if args.apply else (args.output or original.with_name("manifest-earth-ground-roads.json")).resolve()
    if output.parent != original.parent or (not args.apply and (output == original or output.exists())):
        raise ValueError("Output must be a fresh sibling manifest, or use explicit --apply")
    manifest, receipt = classify(original, args.osm)
    data = encoded(manifest)
    if len(data) > 4*1024*1024:
        raise ValueError("Annotated manifest exceeds engine 4MiB cap")
    receipt_path = output.with_suffix(".ground-roads-receipt.json")
    if receipt_path.exists():
        raise ValueError("Preserve existing classification receipt")
    if args.apply:
        backup = original.with_name(original.stem+".before-ground-roads-"+receipt["manifestInputSHA256"][:12]+".json")
        if backup.exists() and digest(backup) != receipt["manifestInputSHA256"]:
            raise ValueError("Existing backup differs")
        if not backup.exists():
            backup.write_bytes(original.read_bytes())
        receipt["backup"] = str(backup)
    receipt["output"] = str(output)
    receipt["manifestOutputSHA256"] = hashlib.sha256(data).hexdigest()
    receipt_bytes = encoded(receipt)
    if len(receipt_bytes) > 32*1024*1024:
        raise ValueError("Classification receipt byte budget")
    receipt_path.write_bytes(receipt_bytes)
    output.write_bytes(data)
    print(json.dumps({"manifest": str(output), "receipt": str(receipt_path), "selected": receipt["selected"], "reasons": receipt["reasons"]}))


if __name__ == "__main__":
    main()
