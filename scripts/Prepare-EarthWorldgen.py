"""Build a local, georeferenced Worldgen region from existing Earth source caches.

No downloads and no game-directory writes. Requires a separate CWR-Worldgen
checkout and its Python dependencies. Stock game assets remain local references.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import subprocess
import sys

from PIL import Image


def cached_height(cache: Path, lat: float, lon: float, images: dict) -> float:
    # Match the Terrarium RGB-to-metres contract; only fully cached bilinear
    # neighborhoods qualify. A missing tile falls back to a coarser cached zoom.
    for zoom in range(12, 3, -1):
        span = (1 << zoom) * 256
        px = (lon + 180) / 360 * span - .5
        py = (1 - math.asinh(math.tan(math.radians(lat))) / math.pi) / 2 * span - .5
        ix, iy = math.floor(px), math.floor(py)
        samples = []
        for dy, dx in ((0, 0), (0, 1), (1, 0), (1, 1)):
            x, y = (ix + dx) % span, iy + dy
            tile = cache / f"terrarium-z{zoom}" / f"{x // 256}-{y // 256}.png"
            if tile not in images:
                if not tile.is_file():
                    images[tile] = None
                else:
                    if tile.stat().st_size > 2 * 1024 * 1024:
                        raise ValueError(f"Oversized cached elevation tile: {tile}")
                    with Image.open(tile) as image:
                        if image.size != (256, 256):
                            raise ValueError(f"Invalid cached elevation dimensions: {tile}")
                        images[tile] = image.convert("RGB")
            image = images[tile]
            if image is None:
                break
            r, g, b = image.getpixel((x % 256, y % 256))
            samples.append(r * 256 + g + b / 256 - 32768)
        if len(samples) == 4:
            fx, fy = px - ix, py - iy
            return ((samples[0] * (1 - fx) + samples[1] * fx) * (1 - fy)
                    + (samples[2] * (1 - fx) + samples[3] * fx) * fy)
    raise ValueError(f"No cached elevation neighborhood at {lat},{lon}; prepare terrain first")


def prepare(cache: Path, output: Path, bbox: list[float], cells: int) -> dict:
    south, west, north, east = bbox
    if not (-84 <= south < north <= 84 and -180 <= west < east <= 180):
        raise ValueError("Invalid supported, non-dateline bounding box")
    if north - south > .1 or east - west > .1:
        raise ValueError("Local preparation is limited to 0.1 degrees per axis")
    output.mkdir(parents=True, exist_ok=False)
    elements = {}
    sources = []
    for kind in ("features", "buildings"):
        for a in range(math.floor(south / .02), math.floor(north / .02) + 1):
            for b in range(math.floor(west / .02), math.floor(east / .02) + 1):
                source = cache / kind / f"{a}_{b}.json"
                if not source.is_file():
                    continue
                payload = source.read_bytes()
                if len(payload) > 4 * 1024 * 1024:
                    raise ValueError(f"Oversized source cell: {source}")
                document = json.loads(payload)
                rows = document.get("elements")
                if document.get("remark") or not isinstance(rows, list) or len(rows) >= 6000:
                    raise ValueError(f"Incomplete source cell: {source}")
                sources.append({"path": str(source), "sha256": hashlib.sha256(payload).hexdigest(),
                                "elements": len(rows)})
                for row in rows:
                    key = (row.get("type"), row.get("id"))
                    if key[0] not in ("node", "way", "relation") or not isinstance(key[1], int):
                        raise ValueError(f"Invalid OSM element in {source}")
                    previous = elements.get(key)
                    if previous is None or len(json.dumps(row)) > len(json.dumps(previous)):
                        elements[key] = row
    if not elements:
        raise ValueError("No cached OSM source cells in this region")
    if len(elements) > 16000:
        raise ValueError("Local generation limit: 16000 source elements")
    osm = output / "osm.json"
    osm.write_text(json.dumps({"version": .6, "elements": list(elements.values())}), encoding="utf-8")
    heights = []
    images = {}
    # Worldgen's image row zero is world Z zero (south), not north-up GIS.
    for z in range(cells):
        lat = south + z / cells * (north - south)
        for x in range(cells):
            lon = west + x / cells * (east - west)
            heights.append(cached_height(cache, lat, lon, images))
    image = Image.new("F", (cells, cells))
    image.putdata(heights)
    image.save(output / "heightmap.tiff")
    receipt = {"bbox": bbox, "cells": cells, "sourceCells": sources,
               "heightMinimum": min(heights), "heightMaximum": max(heights),
               "elevation": "Existing cached Terrarium tiles; no network requests",
               "attribution": "OpenStreetMap contributors, ODbL 1.0; https://www.openstreetmap.org/copyright",
               "coverage": "Only supplied cached cells; missing areas are not populated"}
    (output / "SOURCE-RECEIPT.json").write_text(json.dumps(receipt, indent=2), encoding="utf-8")
    return receipt


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--worldgen-source", type=Path, required=True)
    parser.add_argument("--cache", type=Path, default=Path(os.environ["LOCALAPPDATA"]) / "OpenPoseidon/world-stream")
    parser.add_argument("--output", type=Path, required=True, help="New local lab directory, outside the installed game")
    parser.add_argument("--bbox", nargs=4, type=float, default=[49.6035, 6.12, 49.6165, 6.14])
    parser.add_argument("--cells", type=int, choices=(64, 128, 256), default=128)
    parser.add_argument("--asset-root", type=Path, required=True)
    parser.add_argument("--prepare-only", action="store_true")
    args = parser.parse_args()
    source = args.worldgen_source.resolve()
    if not (source / "src/cwr_worldgen/cli.py").is_file():
        raise ValueError("CWR-Worldgen source checkout missing")
    output = args.output.resolve()
    asset_root = args.asset_root.resolve()
    if output == asset_root or asset_root in output.parents:
        raise ValueError("Generate outside the installed game; deployment is a separate operation")
    receipt = prepare(args.cache.resolve(), output, args.bbox, args.cells)
    if args.prepare_only:
        print(json.dumps(receipt, indent=2))
        return
    south, west, north, east = args.bbox
    width = math.radians(east - west) * 6371000 * math.cos(math.radians((south + north) / 2))
    env = os.environ.copy()
    env["PYTHONPATH"] = str(source / "src") + os.pathsep + env.get("PYTHONPATH", "")
    command = [sys.executable, str(Path(__file__).with_name("Run-EarthWorldgen.py")), str(source),
               "milestone4", "--output", str(output / "generated"),
               "--name", "earth_lux_worldgen", "--display-name", "Earth Luxembourg Worldgen",
               "--cells", str(args.cells), "--cell-size", str(width / args.cells),
               "--heightmap", str(output / "heightmap.tiff"), "--input-mode", "meters",
               "--heightmap-grid", "game-terrain-vertices", "--bbox", *map(str, args.bbox),
               "--osm-json", str(output / "osm.json"), "--asset-root", str(asset_root),
               "--forest-tree-spacing", "35", "--cache-dir", str(output / "generator-cache")]
    (output / "generator-command.json").write_text(json.dumps(command, indent=2), encoding="utf-8")
    print("Generating from local cached sources; original game archives are read-only.", flush=True)
    subprocess.run(command, env=env, check=True, timeout=600)
    if not (output / "generated/manifest.json").is_file():
        raise RuntimeError("Generator exited without a world manifest")
    manifest = json.loads((output / "generated/manifest.json").read_text(encoding="utf-8"))
    status = manifest.get("final_validation", {}).get("status", "unspecified")
    print(f"Upstream final validation: {status}. Output is a presentation fixture until accepted.")


if __name__ == "__main__":
    main()
