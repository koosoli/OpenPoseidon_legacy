# Open Poseidon Engine

<p align="center">
  <img src="screenshots/OpenPoseidonEngine.png" alt="Open Poseidon Engine logo" width="360">
</p>

Open Poseidon Engine brings modern engine features to Operation Flashpoint / Arma:
Cold War Assault: WGPU/Vulkan rendering, a whole-Earth world you can fly from the
ground into space, volumetric fire, smoke, fog and clouds, an FFT ocean with
underwater life, Box3D physics with ragdolls, new infantry animations,
experimental Arma 1, 2 and 3 missions with their own menus, and squad mates you
can talk to through a language model. It is a community continuation of the
original engine, with expanded gameplay, terrain editing and content support.
It requires your own game data and is not a complete game or an official
Bohemia Interactive product.

[![Sponsor on GitHub](https://img.shields.io/badge/Sponsor-GitHub%20Sponsors-ea4aaa?logo=githubsponsors&logoColor=white)](https://github.com/sponsors/koosoli)
[![Buy Me a Coffee](https://img.shields.io/badge/Buy%20me%20a%20coffee-support-ffdd00?logo=buymeacoffee&logoColor=black)](https://buymeacoffee.com/koosoli)

> [!TIP]
> **Don't want to compile? Ready-to-play Windows builds are waiting for you.**
> Sponsors get access to our private builds repository,
> [OpenPoseidon-Builds](https://github.com/OpenPoseidon/OpenPoseidon-Builds/releases),
> with tested, precompiled releases. Become a member through
> [GitHub Sponsors](https://github.com/sponsors/koosoli) for a small monthly
> contribution: you get the builds and directly help fund the project's
> development, testing and build distribution. Building from source stays free,
> and your rights under the software licences are the same either way.

## Playing a precompiled build

1. **You need the original game.** Open Poseidon runs on top of your own copy of
   *Arma: Cold War Assault* (Steam or GOG) or *Operation Flashpoint*. It does not
   include the game's data, so the game must be installed first.
2. **Unpack the download** and **drag its contents into the game's installation
   folder**, next to the game's own `.exe`: `OpenPoseidon.exe`,
   `wgpu_renderer.dll` and the `assets` folder. Nothing of the original game is
   overwritten, and the original game still starts as before.
3. **Start `OpenPoseidon.exe`** from that folder. If Windows reports a missing
   `MSVCP140` or `VCRUNTIME140` file, install Microsoft's
   [Visual C++ Redistributable (x64)](https://aka.ms/vs/17/release/vc_redist.x64.exe).
4. **Optional, for the modern main and options menus:** install the free
   *Arma: Cold War Assault Remastered Demo* from Steam, then run
   `assets\Install-Menus.cmd` once. It copies the Demo's menu resources into the
   `assets` folder. Without it the game uses the classic menus.

To play Arma 1, 2, 3, DayZ or Reforger content you also need those games
installed; their files are loaded in place from their own folders.

## Engine features

### Worlds and content

> **No conversion, no copying.** Arma 1, 2, 3, DayZ and Reforger content loads
> straight from the original game's own installation folder. Point the engine at
> your installed game and play it in place: nothing is converted into a separate
> copy beforehand, and none of those games' files ship with Open Poseidon.

- **Arma 1, 2 and 3 missions (experimental):** missions and campaigns from your
  own Arma installations start from their native main menus, pause screens and
  mission dialogs, with their own weapons, sounds, voices, animals, scripted
  state machines and conversations. Arma 1 support is the furthest along: its
  missions bring their own weapons, sights, magazines, shot sounds, vehicles with
  their own sounds and gun points, the Arma 1 equipment slots and its stringtables,
  and Arma 1 units, vehicles and objects can be placed in the mission editor. Many missions play from start to finish; others
  still stop at missing script commands or content.
- **Whole-world rendering:** fly from the ground to orbit over the whole Earth,
  built from real elevation and imagery streamed at runtime. Mountains cast
  shadows across the ground, the sea is drawn out to the horizon, valleys fill
  with fog and snowlines follow the season. A real-time Moon has its own relief and
  craters, and the Earth hangs in its sky. A scripted ICBM can be flown across the
  globe with a follow camera.
- **Arma 1, 2, 3, Reforger and DayZ terrains:** locally installed worlds load
  with their own buildings, vegetation, roads, materials and far-distance trees.
  Reforger worlds stream natively. Imported terrain opens in the native OFP mission
  editor with OFP units.
- **Island Editor and OFP Fusion:** a terrain editor with no object limit, and an
  archipelago built from all five locally installed classic islands.

### Rendering

- **Vulkan rendering through WGPU:** Vulkan support on Windows and Linux,
  alongside Direct3D 12 on Windows. The active API depends on the platform,
  GPU and driver.
- **GTAO:** screen-space ambient occlusion and bent-normal ambient shading,
  with live developer controls and diagnostic views.
- **Global illumination (GI):** a local irradiance probe grid combines indirect
  sky lighting with coloured sunlight bounced from nearby surfaces.
- **Lighting and shadows:** dynamic sun/local-light shadows, contact shadows,
  muzzle illumination, atmospheric light scattering and script-created lights
  (Arma 2/3 light commands).
- **Vehicle paint and reflections:** clear-coat paint that reflects the sky and
  the lit ground, with screen-space reflections; glass, chrome and instruments
  are told apart from paint.
- **Volumetric clouds and procedural sky:** overcast controls shared with Zeus,
  stars, the Milky Way and cloud shadows affecting terrain, objects, grass and water.
- **Layered volumetric fog:** adjustable lower/upper heights, density, patchiness,
  valley/mountain presets, live controls, reset and a legacy-fog option. Local
  light scattering supports night lights and the player's torch.
- **GPU-driven trees:** dense forests, including Arma 3 and DayZ vegetation, are
  culled and drawn on the GPU.
- **Upscaling:** AMD FSR 1 and an optional local NVIDIA DLSS integration.
  The proprietary NGX SDK/runtime is not bundled; distributable builds disable
  DLSS. Its availability depends on build configuration and compatible hardware.

### Fire, smoke and destruction

- **Volumetric fire and smoke:** a new fire and smoke system. Explosions start with a
  fireball, wrecks burn with flames that lean in the wind, and black smoke columns
  rise, drift and linger long after the fire. Smoke has buoyancy and wind
  response, self-shadowing and scene shadows, and room/portal-aware containment
  against walls, floors and ceilings, with flow through doorways. Containment
  depends on the building geometry/topology available to the engine.
- **Volumetric impact craters:** explosions dig real craters into the terrain,
  with thrown-out earth around the rim.
- **Holes in walls:** bullets and explosions break physical holes into walls.
- **Destructible terrain and editor:** brush deformation, small cuts,
  horizontal caves/tunnels, sinkholes, straight and tank trenches, and excavation
  selection/rotation/deletion. This extends terrain holes; it is not a general
  voxel-world simulation.
- **Combat effects:** muzzle flashes that light the shooter and the surroundings,
  ground explosions, snow bullet tracks and helicopter ground downwash particles.

### Water, weather and wildlife

- **Tidewater ocean:** FFT waves, shoreline wetness, reflections, foam, wakes,
  spray, underwater rendering and caustics; projectile/explosion and helicopter
  interactions. Boats are rigid bodies that float at a real draft on the drawn
  waves and leave ripples.
- **Procedurally generated wildlife and coastline:** schools of fish gather around
  you under water. The sea floor has real depth, rocks and corals, and beaches
  get rippled sand, stones and driftwood. Surf, gulls, terns and whales are heard
  around the coast.
- **Procedural grass:** travelling gust fronts, spatial wind variation,
  configurable coverage and player/vehicle/explosion/downwash interactions.
- **Weather and puddles:** rain collects in puddles with ripples and drains away
  again, with wet ground and clothing, surface snow and terrain contact depressions.
- **Snow depth and deformation:** compressed foot/vehicle tracks and projectile
  marks in depth-bearing ground snow, alongside roof and vegetation snow cover.
- **Mud:** MudRunner-style mud with ruts, rolling resistance and mud thrown from
  wheels and tank tracks.

### Soldiers, AI and gameplay

- **NPC chat (experimental):** talk to your squad, nearby soldiers, civilians and
  HQ by typing or by voice. A language model of your choice answers in character,
  using a live picture of the situation and the mission briefing. Hosted
  providers and fully local models (Ollama or any OpenAI-compatible server) are
  supported. Replies are spoken in 3D at the speaker, or over the radio. Orders
  you give in plain words (follow, hold, move, cover, engage, get in and more)
  run through the game's own commands; model output is never run as script. API
  keys stay on your computer.
- **New infantry animations:** seamless reloading while walking, leaning around
  corners (Q/E), jumping with a soft landing, swimming, and diving and surfacing.
- **Improved AI:** gunfire hearing/reporting, immediate orders and near-miss
  suppression. Soldiers remember where danger came from and search there, turn
  on whoever is shooting at them, look out from cover, avoid cover where a
  squad mate was hit and run from live grenades. Optional gradual spotting and
  cover/flank/search behavior. Behavior switches are configuration-dependent;
  this is not a universal new navigation system or complete replacement AI.
- **Physical inventory (single player):** open with **Tab ↹** on foot (**O** in vehicle control seats) for vicinity cargo,
  drag/drop, equipped slots and weight/volume budgets; **K** opens inventory
  settings, including optional bearing callouts, a bearing readout and
  grenade-launcher ground-range estimates. Includes 17 original realistic
  equipment icons; addon pictures and labelled tiles remain fallbacks.
  Multiplayer keeps classic Gear; Dev Tools and Zeus are disabled in multiplayer.
- **Box3D physics and articulated ragdolls:** jointed character bodies, death and
  explosion reactions, and bullet reactivation of supported runtime corpses.
  Automatic ragdolls default to on, with a Physics developer-tab toggle.
- **Firing from vehicles (FFV):** personal-rifle fire from configured passenger
  seats, aiming arcs, reload and action-menu Ready/Stow controls. Stock Jeep
  coverage is validated; other seats, weapon families and multiplayer are limited.
- **Steam Audio (default audio backend):** Valve's Steam Audio places every
  3D sound around you with binaural (HRTF) spatialisation, so headphones tell
  you where a shot or an engine is. Sounds are muffled by terrain and buildings
  between you and the source, rooms and surroundings add their own reverb, and
  diving brings underwater sound. The sea has its own soundscape: surf wave by
  wave, gulls, terns, whales and boat engines. The classic OpenAL backend
  remains available.

### Engine and tools

- **Satellite terrain textures (satmaps):** authored satellite layers and an
  automatic classic-world far-colour bake incorporating static roofs/vegetation.
  This is a ground colour representation, not a replacement for live object LODs.
- **Local content browser and editor:** terrain discovery for installed Arma
  1/2/3, DayZ and Reforger content, partial format conversion and selected vehicle
  bridges.
- **Streaming and developer tools:** geometry/residency work, GPU timings,
  debug views, test harnesses and Zeus freefly/unit placement.
- **Multithreading:** enkiTS worker jobs for selected terrain/cache, texture
  conversion and audio work, plus asynchronous streaming and pipeline builds.
  This does not imply that every simulation/rendering operation runs on its own thread.

## Development screenshots

These captures illustrate development builds using locally installed game
content. Imported maps/assets are not included with the engine source.

| Arma 3 mission with its own pause menu | Volumetric fire, smoke and impact craters |
| --- | --- |
| ![An Arma 3 mission paused on its native menu, with an Arma 3 vehicle and base](screenshots/arma3-mission.png) | ![A burning wreck under a black smoke column beside a fresh impact crater](screenshots/volumetric-crators-newfire-smoke.png) |
| **Whole-world rendering** | **The Moon with the Earth in its sky** |
| ![The Earth seen from orbit, with the day/night line across Europe](screenshots/wholeworldrendering.png) | ![Cratered lunar ground with the Earth above the horizon](screenshots/moon.jpg) |
| **Procedural fish schools under water** | **Rain puddles on a beach with driftwood** |
| ![Schools of fish over a rocky sea floor below the wave surface](screenshots/wildlife.jpg) | ![A rain puddle with ripples on rippled beach sand, with stones and driftwood](screenshots/puddles.png) |
| **Arma Reforger terrain** | **Tidewater ocean** |
| ![A Reforger landscape of fields, forests and coast from the air](screenshots/reforger.jpg) | ![Tidewater ocean waves and shoreline](screenshots/newocean.png) |
| **Wind-driven procedural grass** | **Layered volumetric fog** |
| ![Procedural grass across a rolling meadow](screenshots/newgrass.png) | ![Low fog layers across island valleys](screenshots/volumetricfog.png) |
| **Snow depth and vehicle tracks** | **Destructible terrain and craters** |
| ![Snow cover with compressed vehicle tracks](screenshots/volumetricsnow.png) | ![Deep terrain deformation near a village road](screenshots/destrucableterrain.jpg) |
| **GTAO and interior shading** | **Night lighting and shadows** |
| ![Ambient occlusion and directional light inside a building](screenshots/GTAO.png) | ![Village lights and shadows in rainy night conditions](screenshots/newshadowsystem.png) |
| **Box3D physics test scene** | |
| ![Box3D wall and rigid-body test scene](screenshots/box3d.png) | |

## Current limitations

Features are at different stages of acceptance. Dense/aerial fog can still show
patterns. Ragdoll tests cover
37 locally present stock models; two Camel pilot models retain post-hit settling
issues. Posture, saved-corpse reactivation, network ownership and physics budgets
also limit ragdoll admission. Arbitrary addon rigs are not universally supported.
Weather materials and modern-game material/LOD conversion still need refinement.
Soft-ground prototypes do not provide general soft-body vehicle physics.
Arma 1, 2 and 3 missions are experimental: some still stop at missing script
commands or content, and they need your own Arma installations. Whole-world
rendering streams public elevation and imagery data and needs an internet
connection. NPC chat needs a language-model provider or a local model server of
your choice.

## Content and building

To play Cold War Assault, use your own legally acquired installation. Game
archives, maps, models, textures, sounds and animation addons are not supplied
by this source release. Included project artwork and omitted binary test fixtures
are documented in [ASSETS.md](ASSETS.md). Original-game and modern-game compatibility remains
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
