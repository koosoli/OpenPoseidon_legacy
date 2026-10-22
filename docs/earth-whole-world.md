# Whole world

Whole-world mode streams the real Earth around a chosen place: elevation, satellite imagery and,
optionally, OpenStreetMap buildings and roads. It is entered from the main menu (**WHOLE WORLD**) and
runs in its own engine process, like Local maps.

## The menu

* **Place:** a preset, or any latitude (±84°), longitude and altitude (500 m to 30,000 km).
* **Imagery:** built-in (the NASA globe plus the prepared Luxembourg caches), Esri World Imagery,
  Mapbox Satellite or MapTiler Satellite (your API key), or a custom tile URL.
* **Elevation:** AWS Terrain Tiles (free, the default), Mapbox Terrain-RGB (your key), or custom.
* **3D buildings, roads:** off, or OpenStreetMap through the Overpass API.
* **Start:** the free-flight camera, or on foot as the mission's soldier.

Choices are saved in `%LOCALAPPDATA%/OpenPoseidon/world-stream/providers.cfg`. The API key is stored
there in plain text and never written to a log.

## providers.cfg

```
imagery = none | esri | mapbox | maptiler | custom
imagery_url = https://.../{z}/{x}/{y}.jpg?key={key}     (custom only)
imagery_key = ...
imagery_max_zoom = 19
elevation = terrarium | mapbox | custom
elevation_url = https://.../{z}/{x}/{y}.png             (custom only)
elevation_format = terrarium | terrain-rgb               (custom only)
elevation_key = ...
content = none | overpass
content_url = https://overpass-api.de/api/interpreter
```

URL templates are web-mercator XYZ: `{z} {x} {y}`, `{-y}` (TMS rows), `{q}` (quadkey), `{s}`
(a/b/c), `{key}`. Each value has an environment override (`POSEIDON_EARTH_IMAGERY_PROVIDER`,
`POSEIDON_EARTH_IMAGERY_URL`, `POSEIDON_EARTH_IMAGERY_KEY`, `POSEIDON_EARTH_ELEVATION_*`,
`POSEIDON_EARTH_CONTENT_OVERPASS`), see `engine/Poseidon/World/Terrain/EarthProviders.hpp`.

## What streams, and how detailed

* **Imagery:** a 2048² near atlas (warped toward the camera) and a 4096² far atlas warped toward the
  near window, filled from nested provider rings, each ring 4× wider at its own zoom. Measured over
  Manhattan at 450 m: zoom 18 under the camera, zoom 9 at the horizon. Tiles already on disk are shown
  first; downloads replace them as they arrive. Transient failures are retried. Downloads reuse
  warm connections and TLS sessions, sixteen at a time in a rolling pool, and the tiles under the
  camera are fetched and shown before the wide outer rings.
* **Elevation:** a 257² near patch: 6.25 m grid (zoom 14) below 1.5 km altitude, 25 m (zoom 12) below
  8 km, 100 m (zoom 10) above; a curved horizon to the visible radius.
* **OpenStreetMap:** buildings extruded from `height` / `building:levels` (up to 900 m), roads,
  waterways, forests, per 0.02° cell, a 3×3 cell neighbourhood around the camera. Opt-in because the
  public Overpass server is shared: one request at a time, failed cells wait a minute, every cell is
  cached. Point `content_url` at your own Overpass instance for heavy use.

## Flying to space and to the moon

Free flight climbs continuously from the ground to orbit: the air in front of the globe (aerial
perspective), the sunlight on each point of the globe and the cloud layers all blend with altitude,
and the streamed ground fades into the globe image instead of ending at a rectangle. Above 12 km the
ground is lit by the sun through its own column of air, so the terminator reddens and the day side
stays daylit while the starting place is in night; ground-level views are unchanged.

From orbit the moon is a body 384,400 km from the Earth's centre (real ephemeris direction): it shows
parallax, grows as you approach, and free flight can travel to it: speed follows the distance to the nearer
of the Earth and the moon, and the camera levels against whichever is nearer.

**The moon frame** (`EarthMoonFrame.hpp`). Past the midpoint the world switches from the Earth origin's
tangent frame to a tangent frame on the moon, and back on the way home. In the Earth frame the moon is
~3.8e8 m out, where a float steps 32 m: the camera shook and nothing could stand there. In the moon frame
the camera and the surface are small numbers again, and the frame re-centres across the moon as the Earth
frame does across the Earth, so free flight can orbit it. The Earth (globe, clouds, atmosphere) is drawn
through the frame transform; its near ground, grass, trees and water are not drawn from there.

On the moon:

* **Ground.** The moon's sphere (radius 1,737.4 km) is the ground for walking, vehicles, collisions and the
  dev tools' *Zeus: beam player here*. The relief and craters are shading only, so the silhouette and the
  ground agree. There is no sea.
* **Gravity** is the moon's, 1.62 m/s^2, for everything the simulation drops (`G_CONST`): a jump rises
  about six times as high and hangs about six times as long. Back in the Earth frame it is 9.8066 again.
* **Surface.** NASA's LRO Wide Angle Camera global mosaic (`assets/earth/moon/moon_albedo.pmoon`, 4096 x
  2048, built by `tools/sky/make_moon_albedo.py`), with procedural grain and craters below its ~2.7 km
  texels. Without the file the moon falls back to procedural maria.
* **The Moon** in the menu's place list starts in free flight 400 m over the near side, with the sun low
  and the Earth overhead; the player stands on the surface below, ready to be beamed or walked about.

The Go/menu altitude stays limited to 30,000 km; flight itself reaches 450,000 km.

## Desert Island

`python scripts/Prepare-EarthIsland.py --wrp "<game>/Worlds/intro.wrp" --name desert --lat 45.40 --lon -7.80`
reads OFP's Desert Island from your installation and writes its heights and ground image into the cache
(`islands/desert`). Whole-world mode then shows it in the open Bay of Biscay; the menu has a preset for it.
Any OFP world works the same way (heights and ground textures only; objects are not carried over).

## Script commands

Positions use the usual `[x, y(north), height]` order; the chosen place is map `[6400, 6400]`.
Outside whole-world mode every command answers `[]` / `false`.

| Command | Result |
| --- | --- |
| `earthLocation` | `[latitude, longitude, altitude, revision]` of the geographic origin |
| `earthPosToGeo [x, y]` | `[latitude, longitude]` |
| `earthGeoToPos [lat, lon]` | `[x, y, 0]`, or `[]` on the far side of the globe |
| `earthGoTo [lat, lon, alt]` | `true` when the world was moved there; move units to `[6400, 6400]` |
| `earthGroundHeight [x, y]` | `[height]` of the streamed ground, `[]` where none is resident yet |

## Offline region packs

```
python scripts/Prepare-EarthRegion.py --lat 46.537 --lon 7.962 --radius-km 10 [--osm] [--street]
```

Downloads one area's elevation, imagery (from `providers.cfg`, or `--imagery`) and, with `--osm`,
OpenStreetMap cells into the same cache the engine reads before any network request. That area then
renders and plays without a connection.

## Terms

Elevation (AWS Terrain Tiles) and OpenStreetMap (ODbL, credit "© OpenStreetMap contributors") allow
this use. Commercial imagery services set their own terms, and several restrict bulk or offline
caching; checking them for your use is up to you.

## Not done yet

* Buildings are untextured extrusions; roofs and walls do not take colour from the imagery.
* The imagery near the camera tops out where the provider and the tile budget allow (zoom 17-18).
* AI and vehicles work on the streamed ground where it is resident, but there is no navigation mesh
  or road network for AI driving on Earth.
* The disk cache for imagery is not trimmed.
