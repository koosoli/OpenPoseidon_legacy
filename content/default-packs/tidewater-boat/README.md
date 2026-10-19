# @OP_TidewaterBoat

Tidewater's 8.2 m Downeast lobster boat as an OFP ship, `OP_TidewaterBoat`. Spawn it from the dev
panel: Zeus tab, *Spawn at camera aim*, **Spawn Tidewater boat ahead** (or the `OP_TidewaterBoat`
vehicle preset).

## Provenance

The boat is **code, not an asset**. [Tidewater](https://github.com/dgreenheck/tidewater) (an
open-source WebGPU ocean demo, MIT, (c) 2026 DRG Software Solutions LLC) builds it procedurally in
`src/world/BoatModel.js` and `src/world/boat/*.js` from hull lines, lofts and primitives; it ships
no model or texture file for it. No game's content is used.

`source/tidewater_boat_mesh.bin.gz` is what that code builds, exported once with
`scripts/default-content/export_tidewater_boat.mjs` from Tidewater commit
`4811ba48d795197de5621985f404e765c0b7c0ef`, so the engine build needs no Node:

```
node scripts/default-content/export_tidewater_boat.mjs <tidewater checkout> content/default-packs/tidewater-boat/source/tidewater_boat_mesh.bin.gz
```

`scripts/Build-TidewaterBoat.ps1` (run by `Build-DefaultContent.ps1`) turns it into the model, two
textures and `config.cpp` with `scripts/default-content/generate_tidewater_boat.py`.

## What carries over

* **Hull and handling**: the config's `class TidewaterHull` is Tidewater's `BoatController`
  hydrostatics -- its 32 waterplane patches, inertia, 26 kN bollard pull, 16 m/s pitch speed, rudder
  in the propeller race, keel stations -- integrated by `ShipTidewater.cpp` at 120 Hz on the drawn
  Tidewater sea, unscaled. Other ships keep Tidewater's hull scaled to their size.
* **Look**: Tidewater paints the boat in its shaders. Here every part takes its vertex colour (one
  flat cell of `boat_palette`), the hull its paint by height (`boat_hull`: antifouling, boot stripe,
  white topsides, cove stripe), the radar, chart plotter and gauges one still frame of their screens
  (`boat_displays`), and the traps their wire grid and netting as cut-out textures, double-sided.
  The procedural grime, wood grain and non-skid are not reproduced.
* **Water**: Tidewater keeps the sea out of the hull with a mask pass. Here the water shader skips
  surface points inside the open cockpit and wheelhouse (`waterMask[]`, `tw_water.wgsl insideHull`):
  the sole sits 0.35 m over the waterline, under every swell's crest.
* **Deck**: Tidewater's character colliders become the geometry LOD (bulwarks, transom, house walls,
  console, roof, foredeck -- thickened outboard; the deck gear is walked through), its walkable boxes
  the roadway LOD. A man standing on the roadway is carried by the boat
  (`Ship::CarryRiders`), so he can walk the deck under way. He boards and steps out at the helm door
  on the deck; a swimmer climbs in from the water.

## Not carried over

Tidewater's spray, engine audio, mooring lines, the animated screens (radar sweep) and night lighting, the
throttle-lever and propeller animation, and its own deck-walk controller (OFP's infantry walks the deck
instead).

## Testing

`dev-missions/tidewater-boat.Eden` puts you at the helm off the Everon bay beach.
`dev-missions/tidewater-boat-harness.Eden` is the capture run (with `--auto-keys`): drive, step out,
a second soldier takes the helm while you ride and walk the deck, take the helm back.
`POSEIDON_ZEUS_BOAT_AHEAD=<seconds>` presses the Zeus tab's spawn button once; `POSEIDON_BOAT_DIAG=1`
logs the hull's waterline against the sea and the deck riders.
