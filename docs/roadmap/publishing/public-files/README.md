# OpenPoseidon

OpenPoseidon is a community continuation of the Operation Flashpoint / Arma:
Cold War Assault engine source. It adds a WGPU renderer, Tidewater ocean rendering,
developer tools and local content compatibility work. This is a source release;
it is not a complete game or an official Bohemia Interactive product.

## Engine features

- **Box3D physics and articulated ragdolls:** jointed character bodies, death and
  explosion reactions, and bullet reactivation of supported runtime corpses.
  Automatic ragdolls default to on, with a Physics developer-tab toggle.
- **Layered volumetric fog:** adjustable lower/upper heights, density, patchiness,
  valley/mountain presets, live controls, reset and a legacy-fog option. Local
  light scattering supports night lights and the player's torch.
- **GTAO:** screen-space ambient occlusion and bent-normal ambient shading,
  with live developer controls and diagnostic views.
- **Volumetric clouds and procedural sky:** overcast controls shared with Zeus,
  stars and cloud shadows affecting terrain, objects, grass and water.
- **Tidewater ocean:** FFT waves, shoreline wetness, reflections, foam, wakes,
  spray, underwater rendering and caustics; projectile/explosion and helicopter
  interactions, with supported boats following the drawn water surface.
- **Procedural grass:** travelling gust fronts, spatial wind variation,
  configurable coverage and player/vehicle/explosion/downwash interactions.
- **Weather and soft ground:** rain accumulation/drainage, puddle ripples,
  wet-ground/clothing shading, surface snow and terrain contact depressions.
  Stock Jeep and Truck5t wet-mud ruts/rolling resistance are enabled; boot-sole
  normal/parallax relief remains an opt-in prototype.
- **Terrain editor:** small cuts, horizontal caves/tunnels, straight and tank
  trenches, and excavation selection/rotation/deletion. This extends terrain
  holes; it is not a general voxel-world simulation.
- **Combat effects:** revised muzzle flashes with local illumination, snow
  projectile marks and helicopter ground downwash particles.
- **Local content browser and editor:** terrain discovery for installed Arma
  1/2/3, DayZ and Reforger content, partial format conversion and selected vehicle
  bridges. Imported terrain opens in the native OFP mission editor with OFP units.
- **Streaming and developer tools:** geometry/residency work, GPU timings,
  debug views, test harnesses and Zeus freefly/unit placement.
- **AI and swimming:** hearing/reporting/suppression fixes, optional
  spotting/hunting behavior, swimmer movement and boat-route support.
- **Title menu:** a patrol boat navigating large Tidewater swells in afternoon light.

## Current limitations

Features are at different stages of acceptance. Dense/aerial fog can still show
patterns, and strong visible torch shafts remain unaccepted. Ragdoll tests cover
37 locally present stock models; two Camel pilot models retain post-hit settling
issues. Posture, saved-corpse reactivation, network ownership and physics budgets
also limit ragdoll admission. Arbitrary addon rigs are not universally supported.
Weather materials and modern-game material/LOD conversion still need refinement.
Soft-ground prototypes do not provide general soft-body vehicle physics.

## Content and building

To play Cold War Assault, use your own legally acquired installation. Game
archives, maps, models, textures, sounds and animation addons are not supplied
by this source release. Original-game and modern-game compatibility remains
subject to the limitations in [COMPATIBILITY.md](COMPATIBILITY.md).

See [BUILDING.md](BUILDING.md) for the Windows build procedure and
[CONTRIBUTING.md](CONTRIBUTING.md) for contributions. The current Windows WGPU
path is the primary development target. Other platform presets exist but are
not a promise of equivalent runtime support.

The source is governed by [LICENSE](LICENSE), including its additional terms.
Third-party components retain their own licenses and notices. See
[CREDITS.md](CREDITS.md), [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) and
the notices shipped with individual components. Source availability does not
grant rights to proprietary game content or trademarks.
