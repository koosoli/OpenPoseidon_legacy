"""Offline region pack for whole-world mode: download one area's elevation, imagery and (optionally)
OpenStreetMap buildings/roads into the same local cache the engine streams from, so that area renders and
plays without a network connection.

    python scripts/Prepare-EarthRegion.py --lat 40.758 --lon -73.985 --radius-km 10 [--osm] [--street]

What it fetches (all into %LOCALAPPDATA%/OpenPoseidon/world-stream, or --cache):
  * elevation: the configured elevation provider (providers.cfg), zooms 5-8 over the horizon, 10 and 12 over
    the region, 14 over the inner part -- exactly the tile names EarthTerrainStream reads;
  * imagery: the configured imagery provider, nested rings coarse to fine (zoom 7 out to ~600 km, zoom 16
    within ~2.5 km; --street adds zoom 17-18 within ~1 km) -- the names EarthImageryTiles reads;
  * --osm: OpenStreetMap buildings and roads/forests per 0.02-degree cell from the Overpass endpoint in
    providers.cfg (content_url), one request at a time with a pause, into cache/buildings and cache/features
    (hand-prepared cells, which the engine never evicts).

Provider terms are yours to check: AWS Terrain Tiles and OpenStreetMap allow this; several commercial
imagery services restrict bulk or offline caching of their tiles. Keys are never printed.

Keep the URL presets, cache names and selectors in sync with engine/Poseidon/World/Terrain/EarthProviders.hpp,
engine/WgpuRenderer/EarthImageryTiles.cpp and engine/WgpuRenderer/EarthContentSource.cpp.
"""
import argparse
import concurrent.futures
import math
import os
import sys
import time
import urllib.parse
import urllib.request

IMAGERY = {
    "esri": ("https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}", 19, False),
    "mapbox": ("https://api.mapbox.com/v4/mapbox.satellite/{z}/{x}/{y}.jpg90?access_token={key}", 19, True),
    "maptiler": ("https://api.maptiler.com/tiles/satellite-v2/{z}/{x}/{y}.jpg?key={key}", 20, True),
}
ELEVATION = {
    "terrarium": ("https://s3.amazonaws.com/elevation-tiles-prod/terrarium/{z}/{x}/{y}.png", False),
    "mapbox": ("https://api.mapbox.com/v4/mapbox.terrain-rgb/{z}/{x}/{y}.pngraw?access_token={key}", True),
}
AGENT = "OpenPoseidon-EarthRegion/1 (+https://github.com/koosoli/OpenPoseidon)"
MERCATOR = 85.0511287798066


def load_config(cache):
    c = {"imagery": "none", "imagery_url": "", "imagery_key": "", "imagery_max_zoom": "",
         "elevation": "terrarium", "elevation_url": "", "elevation_key": "",
         "content": "none", "content_url": "https://overpass-api.de/api/interpreter"}
    path = os.path.join(cache, "providers.cfg")
    if os.path.exists(path):
        for line in open(path, encoding="utf-8"):
            line = line.strip()
            if not line or line[0] in "#;" or "=" not in line:
                continue
            k, v = (s.strip() for s in line.split("=", 1))
            if k in c and (v or k not in ("content_url",)):
                c[k] = v
    return c


def cache_name(provider_id, url):
    """EarthProviders::CacheName: FNV-1a 64 over the URL template, folded to 32 bits."""
    h = 1469598103934665603
    for byte in url.encode("utf-8"):
        h = ((h ^ byte) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return "%s-%08x" % (provider_id, (h ^ (h >> 32)) & 0xFFFFFFFF)


def expand(template, key, z, x, y):
    q = "".join(str(((x >> (i - 1)) & 1) + 2 * ((y >> (i - 1)) & 1)) for i in range(z, 0, -1))
    return (template.replace("{z}", str(z)).replace("{x}", str(x)).replace("{-y}", str((1 << z) - 1 - y))
            .replace("{y}", str(y)).replace("{q}", q).replace("{s}", "abc"[(x + y) % 3]).replace("{key}", key))


def tiles_around(lat, lon, radius_km, z):
    """Web-mercator tile range covering a square of +-radius around the point."""
    dlat = radius_km / 111.32
    dlon = radius_km / (111.32 * max(0.05, math.cos(math.radians(lat))))
    n = 1 << z

    def to_tile(la, lo):
        la = max(-MERCATOR, min(MERCATOR, la))
        x = (lo + 180.0) / 360.0 * n
        y = (1 - math.log(math.tan(math.radians(la)) + 1 / math.cos(math.radians(la))) / math.pi) / 2 * n
        return int(math.floor(x)), int(math.floor(y))

    x0, y0 = to_tile(lat + dlat, lon - dlon)
    x1, y1 = to_tile(lat - dlat, lon + dlon)
    for y in range(max(0, y0), min(n - 1, y1) + 1):
        for x in range(x0, x1 + 1):
            yield x % n, y


def fetch(url, target, attempts=4):
    if os.path.exists(target) and os.path.getsize(target) > 0:
        return "cached"
    os.makedirs(os.path.dirname(target), exist_ok=True)
    for attempt in range(attempts):
        try:
            request = urllib.request.Request(url, headers={"User-Agent": AGENT})
            with urllib.request.urlopen(request, timeout=30) as response:
                body = response.read()
            if not body:
                raise IOError("empty")
            with open(target + ".part", "wb") as f:
                f.write(body)
            os.replace(target + ".part", target)
            return "new"
        except urllib.error.HTTPError as e:
            if 400 <= e.code < 500 and e.code not in (408, 429):
                return "refused"
        except Exception:
            pass
        time.sleep(0.5 * (attempt + 1))
    return "failed"


def run_tiles(label, jobs, workers=8):
    counts = {"cached": 0, "new": 0, "refused": 0, "failed": 0}
    with concurrent.futures.ThreadPoolExecutor(workers) as pool:
        for i, result in enumerate(pool.map(lambda job: fetch(*job), jobs), 1):
            counts[result] += 1
            if i % 200 == 0:
                print(f"  {label}: {i}/{len(jobs)}", flush=True)
    print(f"{label}: {len(jobs)} tiles -- {counts['new']} downloaded, {counts['cached']} already cached, "
          f"{counts['refused']} refused, {counts['failed']} failed", flush=True)
    return counts


def overpass_query(kind, lat, lon):
    b = "(%.8f,%.8f,%.8f,%.8f)" % (lat * .02, lon * .02, (lat + 1) * .02, (lon + 1) * .02)
    if kind == "buildings":
        return '[out:json][timeout:25][maxsize:33554432];way["building"]' + b + ";out tags geom 2000;"
    selectors = ['way["highway"]', 'way["waterway"]', 'way["natural"~"^(water|wood)$"]',
                 'way["landuse"~"^(forest|reservoir)$"]', 'node["traffic_sign"]', 'node["highway"~"^(stop|give_way)$"]']
    return "[out:json][timeout:25][maxsize:33554432];(" + "".join(s + b + ";" for s in selectors) + ");out tags geom 6000;"


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--lat", type=float, required=True)
    parser.add_argument("--lon", type=float, required=True)
    parser.add_argument("--radius-km", type=float, default=10.0, help="detailed area (default 10 km)")
    parser.add_argument("--osm", action="store_true", help="also OpenStreetMap buildings/roads (Overpass)")
    parser.add_argument("--osm-radius-km", type=float, default=3.0)
    parser.add_argument("--street", action="store_true", help="imagery zoom 17-18 within ~1 km")
    parser.add_argument("--imagery", help="override providers.cfg imagery (esri, mapbox, maptiler)")
    parser.add_argument("--elevation", help="override providers.cfg elevation (terrarium, mapbox)")
    parser.add_argument("--cache", default=os.path.join(os.environ.get("LOCALAPPDATA", "."), "OpenPoseidon", "world-stream"))
    a = parser.parse_args()
    if abs(a.lat) > 84 or abs(a.lon) > 180 or not (0.5 <= a.radius_km <= 100):
        sys.exit("latitude within +-84, longitude within +-180, radius 0.5-100 km")
    cfg = load_config(a.cache)
    if a.imagery:
        cfg["imagery"] = a.imagery
    if a.elevation:
        cfg["elevation"] = a.elevation
    print(f"Region pack around {a.lat}, {a.lon}, radius {a.radius_km} km -> {a.cache}")

    # Elevation (EarthTerrainStream::TileDirectory).
    eid = cfg["elevation"] if cfg["elevation"] in ELEVATION or cfg["elevation"] == "custom" else "terrarium"
    eurl, ekeyed = ELEVATION.get(eid, (cfg["elevation_url"], False))
    if not eurl or (ekeyed and not cfg["elevation_key"]):
        eid, (eurl, ekeyed) = "terrarium", ELEVATION["terrarium"]
    edir = (lambda z: os.path.join(a.cache, f"terrarium-z{z}")) if eid == "terrarium" else \
        (lambda z: os.path.join(a.cache, f"elev-{cache_name(eid, eurl)}-z{z}"))
    jobs = []
    for z, radius in [(5, 600), (6, 600), (7, 300), (8, 150), (10, a.radius_km), (12, a.radius_km),
                      (14, min(a.radius_km, 3.0))]:
        for x, y in tiles_around(a.lat, a.lon, radius, z):
            jobs.append((expand(eurl, cfg["elevation_key"], z, x, y), os.path.join(edir(z), f"{x}-{y}.png")))
    run_tiles(f"elevation ({eid})", jobs)

    # Imagery (EarthImageryTiles::TilePath), when a provider is configured.
    iid = cfg["imagery"]
    if iid in IMAGERY or (iid == "custom" and cfg["imagery_url"]):
        iurl, imax, ikeyed = IMAGERY.get(iid, (cfg["imagery_url"], 19, False))
        if cfg["imagery_max_zoom"].isdigit():
            imax = int(cfg["imagery_max_zoom"])
        if ikeyed and not cfg["imagery_key"]:
            print("imagery: the configured provider needs an API key (providers.cfg imagery_key); skipped")
        else:
            idir = os.path.join(a.cache, "imagery", cache_name(iid, iurl))
            rings = [(7, 600), (8, 400), (9, 200), (10, 120), (11, 60), (12, min(40, 4 * a.radius_km)),
                     (13, min(20, 2 * a.radius_km)), (14, a.radius_km), (15, min(a.radius_km, 5)), (16, min(a.radius_km, 2.5))]
            if a.street:
                rings += [(17, 1.2), (18, 0.6)]
            jobs = []
            for z, radius in rings:
                if z > imax:
                    continue
                for x, y in tiles_around(a.lat, a.lon, radius, z):
                    jobs.append((expand(iurl, cfg["imagery_key"], z, x, y), os.path.join(idir, str(z), f"{x}-{y}.tile")))
            run_tiles(f"imagery ({iid})", jobs)
    else:
        print("imagery: no provider in providers.cfg (choose one in the Whole World menu first); skipped")

    # OpenStreetMap via Overpass (EarthContentSource), serial with a pause: shared public infrastructure.
    if a.osm:
        endpoint = cfg["content_url"] or "https://overpass-api.de/api/interpreter"
        r = max(1, int(math.ceil(a.osm_radius_km / 2.2)))
        clat, clon = int(math.floor(a.lat / .02)), int(math.floor(a.lon / .02))
        counts = {"cached": 0, "new": 0, "refused": 0, "failed": 0}
        for dlat in range(-r, r + 1):
            for dlon in range(-r, r + 1):
                for kind in ("buildings", "features"):
                    cell = f"{clat + dlat}_{clon + dlon}.json"
                    target = os.path.join(a.cache, kind, cell)
                    url = endpoint + "?data=" + urllib.parse.quote(overpass_query(kind, clat + dlat, clon + dlon))
                    result = fetch(url, target, attempts=3)
                    if result == "new":
                        body = open(target, encoding="utf-8", errors="replace").read()
                        if '"elements"' not in body or '"remark"' in body:  # an Overpass error page, not a cell
                            os.remove(target)
                            result = "failed"
                    counts[result] += 1
                    if result == "new":
                        time.sleep(1.0)
        print(f"openstreetmap: {sum(counts.values())} cells -- {counts['new']} downloaded, {counts['cached']} cached, "
              f"{counts['failed'] + counts['refused']} failed  (c) OpenStreetMap contributors, ODbL 1.0")
    print("Done. Whole-world mode reads these before any network request.")


if __name__ == "__main__":
    main()
