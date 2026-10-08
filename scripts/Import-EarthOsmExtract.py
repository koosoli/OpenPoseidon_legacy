"""Offline regional OSM extract -> Worldgen geometry JSON and Earth stream cells.

Requires optional pyosmium, installed separately. No network access, game writes,
terrain requests or generation loop. Outputs retain OpenStreetMap ODbL attribution.
"""
from __future__ import annotations

import argparse
from collections import Counter, defaultdict
from datetime import datetime, timezone
import hashlib
import json
import math
from pathlib import Path

MAX_INPUT = 128 * 1024 * 1024
MAX_SOURCE = 64 * 1024 * 1024
MAX_ELEMENTS = 100_000
MAX_POINTS = 2_000_000
MAX_WAY_POINTS = 32_768
MAX_INPUT_NODES = 8_000_000
MAX_RELATIONS = 30_000
MAX_MEMBERS = 200_000
MAX_CELL_BYTES = 4 * 1024 * 1024
CELL = .02
INTEREST = frozenset(("building", "building:part", "highway", "natural", "landuse", "landcover", "sport", "surface",
                     "waterway", "water", "place", "amenity", "tourism", "historic",
                     "man_made", "barrier", "power", "aeroway", "railway", "leisure",
                     "entrance", "traffic_sign", "bridge", "public_transport"))


def encoded(document: dict) -> bytes:
    return json.dumps(document, ensure_ascii=False, separators=(",", ":"), allow_nan=False).encode("utf-8")


def bounds(points: list[dict]) -> tuple[float, float, float, float]:
    return (min(p["lat"] for p in points), min(p["lon"] for p in points),
            max(p["lat"] for p in points), max(p["lon"] for p in points))


def overlaps(a, b) -> bool:
    return a[0] <= b[2] and b[0] <= a[2] and a[1] <= b[3] and b[1] <= a[3]


def region_cells(bbox):
    # Include exact north/east boundaries, matching the stream's floor addressing.
    return [(a, b) for a in range(math.floor(bbox[0] / CELL), math.floor(bbox[2] / CELL) + 1)
            for b in range(math.floor(bbox[1] / CELL), math.floor(bbox[3] / CELL) + 1)]


def validate_bbox(bbox) -> None:
    s, w, n, e = bbox
    if not all(math.isfinite(x) for x in bbox) or not (-84 <= s < n <= 84 and -180 <= w < e <= 180):
        raise ValueError("Require a finite, non-dateline [south west north east] rectangle")
    height = math.radians(n - s) * 6378137
    width = math.radians(e - w) * 6378137 * math.cos(math.radians((s + n) * .5))
    if width > 12_000 or height > 12_000 or len(region_cells(bbox)) > 64:
        raise ValueError("One bounded region only: <=12 km per axis and <=64 cells")


def read_extract(path: Path, bbox) -> tuple[list[dict], dict]:
    import osmium  # Optional dependency; importing helpers/tests does not require it.

    if not path.is_file() or not 0 < path.stat().st_size <= MAX_INPUT:
        raise ValueError("OSM input must be an existing nonempty file <=128 MiB")
    validate_bbox(bbox)
    relations = {}
    member_refs = set()
    diagnostics = Counter()
    elements = []
    stored_points = 0

    def append(element):
        nonlocal stored_points
        stored_points += len(element.get("geometry", []))
        stored_points += sum(len(m.get("geometry", [])) for m in element.get("members", []))
        if len(elements) >= MAX_ELEMENTS or stored_points > MAX_POINTS:
            raise ValueError("Regional source limit exceeded; choose a smaller rectangle (no truncation)")
        elements.append(element)

    class Relations(osmium.SimpleHandler):
        def relation(self, relation):
            tags = dict(relation.tags)
            if tags.get("type") != "multipolygon" or not INTEREST.intersection(tags):
                return
            members = [{"type": m.type, "ref": m.ref, "role": m.role} for m in relation.members]
            if len(relations) >= MAX_RELATIONS or sum_members[0] + len(members) > MAX_MEMBERS:
                raise ValueError("Country multipolygon member budget exceeded")
            sum_members[0] += len(members)
            relations[relation.id] = {"type": "relation", "id": relation.id, "tags": tags, "members": members}
            member_refs.update(m["ref"] for m in members if m["type"] == "w")

    sum_members = [0]
    print("Pass 1/3: indexing tagged multipolygon references", flush=True)
    Relations().apply_file(str(path))
    way_bounds = {}

    class Ways(osmium.SimpleHandler):
        def node(self, node):
            diagnostics["inputNodesScanned"] += 1
            if diagnostics["inputNodesScanned"] > MAX_INPUT_NODES:
                raise ValueError("Input node/location-cache budget exceeded")
            if node.location.valid() and bbox[0] <= node.lat <= bbox[2] and bbox[1] <= node.lon <= bbox[3]:
                tags = dict(node.tags)
                if INTEREST.intersection(tags):
                    append({"type": "node", "id": node.id, "lat": node.lat, "lon": node.lon, "tags": tags})

        def way(self, way):
            tags = dict(way.tags)
            relevant = bool(INTEREST.intersection(tags))
            if not relevant and way.id not in member_refs:
                return
            if len(way.nodes) > MAX_WAY_POINTS:
                raise ValueError(f"Way {way.id} exceeds geometry budget; no source truncation")
            if not all(n.location.valid() for n in way.nodes):
                # Official border extracts can have unresolved outside-country refs.
                diagnostics["waysWithMissingNodeLocations"] += 1
                return
            geometry = [{"lat": n.lat, "lon": n.lon} for n in way.nodes]
            if not geometry:
                return
            box = bounds(geometry)
            if way.id in member_refs:
                way_bounds[way.id] = box
            if relevant and overlaps(box, bbox):
                append({"type": "way", "id": way.id, "nodes": [n.ref for n in way.nodes],
                        "tags": tags, "geometry": geometry})

    print("Pass 2/3: selecting complete regional ways and nodes", flush=True)
    Ways().apply_file(str(path), locations=True, idx="sparse_mem_array")
    chosen = []
    required = set()
    for relation in relations.values():
        boxes = [way_bounds[m["ref"]] for m in relation["members"]
                 if m["type"] == "w" and m["ref"] in way_bounds]
        if not boxes:
            continue
        box = (min(b[0] for b in boxes), min(b[1] for b in boxes),
               max(b[2] for b in boxes), max(b[3] for b in boxes))
        if not overlaps(box, bbox):
            continue
        # Nested relation expansion is intentionally not guessed. A selected
        # incomplete polygon aborts rather than silently losing a hole/outer.
        if any(m["type"] != "w" or m["ref"] not in way_bounds for m in relation["members"]):
            raise ValueError(f"Selected relation {relation['id']} has unresolved/non-way members")
        chosen.append(relation)
        required.update(m["ref"] for m in relation["members"])
    member_geometry = {}
    member_points = [0]

    class Members(osmium.SimpleHandler):
        def way(self, way):
            if way.id not in required:
                return
            geometry = [{"lat": n.lat, "lon": n.lon} for n in way.nodes]
            member_points[0] += len(geometry)
            if member_points[0] > MAX_POINTS:
                raise ValueError("Selected multipolygon geometry budget exceeded")
            member_geometry[way.id] = geometry

    print(f"Pass 3/3: resolving {len(chosen)} complete regional multipolygons", flush=True)
    if required:
        Members().apply_file(str(path), locations=True, idx="sparse_mem_array")
    for relation in chosen:
        for member in relation["members"]:
            member["geometry"] = member_geometry[member["ref"]]
            member["type"] = "way"
        append(relation)
    diagnostics["sourceGeometryPoints"] = stored_points
    diagnostics["countryMultipolygonsIndexed"] = len(relations)
    return sorted(elements, key=lambda e: (e["type"], e["id"])), dict(diagnostics)


def stream_kind(element: dict) -> str | None:
    tags, kind = element.get("tags", {}), element["type"]
    if kind == "way" and tags.get("building", "no") != "no":
        return "buildings"
    if kind == "node":
        if tags.get("natural") == "tree" or "traffic_sign" in tags or tags.get("highway") in ("stop", "give_way"):
            return "features"
    if kind == "way" and ("highway" in tags or tags.get("waterway") in ("river", "stream", "canal", "drain", "ditch", "riverbank")
                          or tags.get("natural") in ("water", "wood") or tags.get("landuse") in ("forest", "reservoir")):
        return "features"
    return None


def prepare_documents(elements: list[dict], bbox) -> tuple[dict, dict]:
    documents = {}
    cells = defaultdict(list)
    exclusions = defaultdict(list)
    candidates = region_cells(bbox)
    for element in elements:
        if element["type"] == "relation":
            exclusions["multipolygonsUnsupportedByEarthCells"].append(element["id"])
            continue
        kind = stream_kind(element)
        if kind is None:
            continue
        points = element.get("geometry") or [{"lat": element["lat"], "lon": element["lon"]}]
        if kind == "buildings" and (len(points) < 4 or len(points) > 129 or points[0] != points[-1]):
            exclusions["buildingGeometryUnsupportedByEarthCells"].append(element["id"])
            continue
        if kind == "features" and len(points) > 2048:
            exclusions["featurePointLimitInEarthCells"].append(element["id"])
            continue
        box = bounds(points)
        admitted = False
        for lat, lon in candidates:
            tile = (lat * CELL, lon * CELL, (lat + 1) * CELL, (lon + 1) * CELL)
            if not overlaps(box, tile):
                continue
            # Existing readers require every vertex within .06 degrees of the
            # addressed cell. Keep full source geometry; never invent clipped IDs.
            if max(abs(p["lat"] - (lat + .5) * CELL) for p in points) > .06 or max(abs(p["lon"] - (lon + .5) * CELL) for p in points) > .06:
                exclusions["cellGeometryOutsideReaderMargin"].append(f"{kind}/{lat}_{lon}/{element['id']}")
                continue
            cells[kind, lat, lon].append(element)
            admitted = True
        if not admitted:
            exclusions["featuresNotAdmittedToEarthCells"].append(f"{element['type']}/{element['id']}")
    # Empty cells explicitly mean no supported features in this extract/region.
    # No cells outside the requested rectangle are emitted or implied populated.
    for kind in ("features", "buildings"):
        for lat, lon in candidates:
            rows = cells[kind, lat, lon]
            limit = 2000 if kind == "buildings" else 6000
            document = {"version": .6, "generator": "OpenPoseidon offline OSM extract importer",
                        "attribution": "© OpenStreetMap contributors; ODbL 1.0", "elements": rows}
            payload = encoded(document)
            if len(rows) > limit or len(payload) > MAX_CELL_BYTES:
                raise ValueError(f"{kind}/{lat}_{lon}: {len(rows)} elements/{len(payload)} bytes exceeds reader limits; choose smaller/sparser region (no truncation)")
            documents[f"{kind}/{lat}_{lon}.json"] = payload
    return documents, dict(exclusions)


def write_region(path: Path, output: Path, bbox, *, worldgen_only: bool = False) -> dict:
    if output.exists():
        raise ValueError("Output must be a new directory; existing caches and game files are never overwritten")
    elements, diagnostics = read_extract(path, bbox)
    document = {"version": .6, "generator": "OpenPoseidon offline OSM extract importer",
                "copyright": "© OpenStreetMap contributors", "license": "ODbL 1.0",
                "attribution": "https://www.openstreetmap.org/copyright", "elements": elements}
    source = encoded(document)
    if len(source) > MAX_SOURCE:
        raise ValueError("Regional geometry JSON exceeds 64 MiB; choose a smaller rectangle")
    cells, exclusions = ({}, {}) if worldgen_only else prepare_documents(elements, bbox)
    source_receipt = path.with_suffix(".receipt.json")
    upstream = json.loads(source_receipt.read_text(encoding="utf-8-sig")) if source_receipt.is_file() else {}
    sha = hashlib.sha256()
    with path.open("rb") as stream:
        while block := stream.read(1024 * 1024):
            sha.update(block)
    digest = sha.hexdigest()
    if upstream.get("sha256") and upstream["sha256"] != digest:
        raise ValueError("Extract hash differs from its download receipt")
    receipt = {"createdUTC": datetime.now(timezone.utc).isoformat(), "bbox": bbox,
               "source": {"path": str(path.resolve()), "bytes": path.stat().st_size, "sha256": digest,
                          "downloadReceipt": upstream}, "license": "ODbL 1.0",
               "attribution": "© OpenStreetMap contributors; extract by Geofabrik where indicated in source receipt",
               "licenseURL": "https://www.openstreetmap.org/copyright",
               "sourceElements": len(elements), "sourceTypes": dict(Counter(e["type"] for e in elements)),
               "sourceFamilies": {"roads": sum(e["type"] == "way" and "highway" in e.get("tags", {}) for e in elements),
                                  "buildings": sum(e.get("tags", {}).get("building", "no") != "no" for e in elements),
                                  "forestAreas": sum(e.get("tags", {}).get("landuse") == "forest" or e.get("tags", {}).get("natural") == "wood" or e.get("tags", {}).get("landcover") == "trees" for e in elements),
                                  "individualTrees": sum(e.get("tags", {}).get("natural") == "tree" for e in elements)},
               "diagnostics": diagnostics, "cellExclusions": exclusions,
               "worldgenSource": "osm.json", "worldgenSourceSHA256": hashlib.sha256(source).hexdigest(),
               "cellFiles": {name: {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest(),
                                    "elements": len(json.loads(data)["elements"])} for name, data in cells.items()},
               "coverage": "BBox-overlapping full feature geometries; no truncation or geometry clipping. Cells cover only supported features in requested region; multipolygon holes/outers remain complete in osm.json. This is OSM source data, not generated game models or terrain."}
    output.mkdir(parents=True, exist_ok=False)
    (output / "osm.json").write_bytes(source)
    for name, data in cells.items():
        target = output / name
        target.parent.mkdir(exist_ok=True)
        target.write_bytes(data)
    (output / "SOURCE-RECEIPT.json").write_bytes(encoded(receipt))
    (output / "ATTRIBUTION.txt").write_text("© OpenStreetMap contributors. Data licensed under ODbL 1.0.\nhttps://www.openstreetmap.org/copyright\nGeofabrik extract source, timestamp and SHA-256: see SOURCE-RECEIPT.json.\n", encoding="utf-8")
    return receipt


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pbf", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True, help="New external lab directory")
    parser.add_argument("--bbox", type=float, nargs=4, required=True, metavar=("SOUTH", "WEST", "NORTH", "EAST"))
    parser.add_argument("--worldgen-only", action="store_true", help="Explicitly omit Earth stream cells, retaining complete regional Worldgen source")
    args = parser.parse_args()
    try:
        result = write_region(args.pbf.resolve(), args.output.resolve(), args.bbox, worldgen_only=args.worldgen_only)
    except (ValueError, ImportError, RuntimeError, OSError) as error:
        parser.exit(1, f"Import failed: {error}\n")
    print(json.dumps({key: result[key] for key in ("sourceElements", "sourceTypes", "sourceFamilies", "diagnostics")}, indent=2))
    print(f"Wrote {len(result['cellFiles'])} stream cells; exclusions recorded in SOURCE-RECEIPT.json", flush=True)


if __name__ == "__main__":
    main()
