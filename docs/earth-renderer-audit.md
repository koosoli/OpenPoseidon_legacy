# Whole World renderer: audit of open globe and terrain renderers (2026-10-24)

The owner asked for an audit of open-source Earth renderers (GeoLibre, terra, planetary_terrain_renderer,
cesium-native and FlightGear). The questions: what inspires us, what code can and should be ported, and
whether we can use the same datasets (the "nicer buildings"), or at least offer them as options.
Licence compatibility matters: our source is GPL-3.0-or-later, so Apache-2.0, MIT and GPL-2.0-or-later
code can come in; GPL-2.0-only cannot.

## Summary: what to take

| Project | Licence | Take | How |
| --- | --- | --- | --- |
| **cesium-native** (C++) | Apache-2.0 | **Integrate as a library**, not a port: 3D Tiles streaming and LOD selection (`Cesium3DTilesSelection`), glTF reading, quantized-mesh terrain, raster overlays | vcpkg/CMake like our other deps; we implement its renderer-resource interface (load thread → glTF to `WgrMesh`/`WgrTexture`, main thread → register) |
| **planetary_terrain_renderer / bevy_terrain** (Rust, Bevy) | MIT / Apache-2.0 | **Port the ideas, maybe code**: separate *geometry LOD* and *data LOD*, a tile atlas with a tile tree, viewer-relative ellipsoid positions (Taylor series) for precision | Our `EarthTileCache` (2026-10-24) is this design for imagery; the next step is the same for elevation. Its GeoTIFF preprocessor is a model for GEBCO bathymetry |
| **terra** (Rust, wgpu) | Apache-2.0 | **Datasets and compute-shader detail**: NASADEM elevation, Global Forest Change treecover, ETOPO1, Blue Marble NG; fractal detail on the GPU | Its streaming file lists are a ready recipe for the data; the treecover raster beats our imagery-colour forest scan |
| **FlightGear** (C++) | GPL-2.0-or-later | **Data pipeline, not renderer code**: landclass-driven ground (CORINE / ESA WorldCover), osm2city buildings, TerraSync | Its scenery is generated from open data we can use directly; the code is OSG-bound |
| **GeoLibre** (TS, MapLibre/Cesium/deck.gl) | MIT | **Product ideas only**: planetary bodies with their own basemaps (Moon from USGS / OpenPlanetaryMap), atmosphere halo, 3D Tiles on a map | Nothing to port: browser stack |

## Datasets: "the same nice buildings", optional where a key is needed

| Data | Source | Licence / access | For us |
| --- | --- | --- | --- |
| **Google Photorealistic 3D Tiles** (photogrammetry: every building, tree, car) | Google Map Tiles API via cesium-native | Needs the *player's own* API key; Google's terms forbid long-term caching and redistribution | **Optional provider**, off by default, key in `providers.cfg`; never shipped or cached beyond the session's needs |
| **Cesium OSM Buildings** (OSM footprints extruded, 350 M+ buildings, as 3D Tiles) | Cesium ion asset 96188 | Free ion account / token; ODbL data | **Optional provider** through cesium-native; better building shapes than our Overpass extrusion |
| **Overture Maps buildings** (2.3 B footprints, many with height) | overturemaps.org, GeoParquet / PMTiles | ODbL + CDLA-Permissive-2.0, no key | **Default candidate** to replace Overpass for buildings: one static download per region, heights where Overpass has none |
| **GEBCO 2023/2024 bathymetry** | gebco.net | Public domain-like (attribution) | **Should do**: a real sea floor instead of "0 m ocean ground". The boat, camera and fish fixes of today all work round its absence |
| **NASADEM / Copernicus GLO-30** elevation | NASA / ESA | Public domain / free with attribution | Higher-quality elevation than Terrarium tiles at 30 m |
| **ESA WorldCover 10 m** land cover | ESA | CC-BY 4.0 | Ground materials, grass, forest and sand from land cover, not from imagery colour (today's `EarthCanopyTrees` and beach-sand colour tests) |
| **Global Forest Change treecover** | Hansen/UMD | CC-BY 4.0 | Tree density per 30 m (what terra uses) |
| **NASA Black Marble** night lights | NASA (GIBS WMTS, `VIIRS_Black_Marble` 2016) | Public domain | **Done 2026-10-24**: city lights on the night side, zoom 3 in 64 fixed slots of the tile atlas |
| **LRO WAC global mosaic / LOLA** (Moon) | USGS Astrogeology / OpenPlanetaryMap tiles | Public domain | Sharper moon imagery than our 2.7 km map (GeoLibre's planetary basemaps) |

## What we should build, in order

1. **Tile pyramid for imagery: done, and the default since 2026-10-24** (`EarthTileCache`;
   `POSEIDON_EARTH_TILES=0` brings back the window atlases). This is bevy_terrain's "data LOD" and
   Cesium's refine-when-ready rule. It ends the window-atlas reloads, the flicker and the
   near/far/globe seam. Night lights (Black Marble) ride in the same atlas, so they need no binding
   of their own (`POSEIDON_EARTH_NIGHT_LIGHTS=0` turns them off).
2. **Bathymetry (GEBCO) into the elevation stream.** It removes a whole class of open-sea special cases
   (`FlightFloor`, `SeaBedY`, the fish's 14 m nominal floor, the boat being re-grounded).
3. **cesium-native as an optional 3D Tiles layer**: Google photorealistic (player's key) or Cesium OSM
   Buildings (ion token). It sits beside our Overpass buildings, which stay the free default. Integration
   work: the renderer-resource interface, glTF → our mesh/texture path, and the tile selection's camera
   from our frame. This is the largest single visual jump and the "same buildings" the owner saw.
4. **Land cover (ESA WorldCover) for ground materials**, grass and sand; Overture buildings as the
   free default.
5. **Elevation as a tile pyramid too** (bevy_terrain's geometry/data split). That replaces
   `EarthTerrainStream`'s window patches, the per-patch heightmap uploads and their main-thread cost.

## Not worth porting

- terra's and bevy_terrain's *render loops*: both are tied to their own engines (wgpu-standalone /
  Bevy). Our renderer already does CDLOD-like meshes, atmosphere and water.
- FlightGear's OSG scene graph and BTG format: we only want its data sources.
- GeoLibre: a GIS front end.

## Found on the way

- **The star field was mirrored**, on the islands and on Earth alike (`sky.wgsl` `to_celestial` and
  `star_field`). The world frame is left-handed, so `cross(pole, east)` in world components is the
  mirror of the catalogue's right-handed axis. Right ascension ran west along the equator, so Orion
  stood reversed. The galactic centre's position in the Milky Way map (u = 0.738 against RA 266.4
  degrees, 0.740) confirms that the map and the catalogue agree with each other, so one fix covers both.
  The island clock now turns the sky the right way round (the sun's hour angle, negated).
- **We already download bathymetry.** The Terrarium elevation tiles (AWS terrain tiles, Mapzen's joerd: SRTM,
  NASADEM, EU-DEM, ETOPO1/GEBCO-derived bathymetry at sea) carry real sea depths. The zoom-3 tiles in the cache
  read down to -5596 m. The "0 m ocean ground" that `FlightFloor`, `SeaBedY` and the boat and fish work round is
  our own handling, not the data. A real sea floor (item 2) needs no new download, only that handling replaced.
- **Data sources compared.** Imagery: Esri World Imagery (ours, z19, no key), against Bing or Google (Cesium,
  keyed) and Blue Marble (terra, coarse). Esri is the best keyless one. Elevation: Terrarium (ours), against NASADEM
  (terra) and Cesium World Terrain (keyed). Terrarium already merges NASADEM/SRTM with bathymetry. Night: Black
  Marble (now ours). Land cover, tree cover and buildings are where the others have better data (WorldCover, GFC,
  osm2city/Overture, 3D Tiles), as listed above.

## Status (2026-10-24, evening)

- **Sea floor: done.** Terrarium's own bathymetry (zoom <= 10) where the finer zooms fill the sea with 0 m
  (`EarthTerrainStream::WithSeaDepth`); WorldCover decides land from sea at sea level, so polder and marsh stay dry.
- **Land cover: in use.** ESA WorldCover (`EarthLandCover`, range requests on the AWS COGs): where forest stands
  (with the imagery as confirmation), land vs sea at the shore, and no grass on built-up, bare, snow or water.
- **Buildings: Overture first.** `EarthOverture` reads the latest release's PMTiles (OSM + machine-traced
  footprints, heights, floors, roof shapes) and writes the same cells the builder already extrudes; Overpass is the
  fallback. Budget 5000 buildings a cell, 400k triangles.
- **Imagery fallback.** When Esri does not answer, EOX Sentinel-2 cloudless 2016 (CC-BY, to zoom 15), behind a
  circuit breaker.
- **Night lights** (Black Marble) and the **star field mirror**: done earlier the same day.
- **Main-thread installs:** sky-view scan of each patch on a worker, no temporal reset per patch, building/feature
  vertices prepared on the stream workers, tree models placed by difference.

Still open: elevation as a tile pyramid (item 5), cesium-native 3D Tiles (item 3), WorldCover for ground materials
in the shader (sand on bare ground; needs a land-cover texture per patch).
