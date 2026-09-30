# Stock M16 visible flash: warm authored lobes and bounded emission

Status: source candidate; GPU composition, combined game build and installed
real-shot appearance checks remain pending. This supplements the actual local
light in `MUZZLE-FLASH-LOCAL-LIGHT-20260930.md`.

Offline inspection of the installed original `DTA/Data3D.pbo` M16 proxy finds
the `zasleh` selection with 22 points and `data\zasleh_front.01.paa` /
`data\zasleh_side.01.paa` sections across its visible/inside-view LODs. The
front flags are `0x34120`, side `0x3c120`: animated, ordered alpha, no shadow,
with the authored clamp flags. `AnimationAnimatedTexture::SetPhase` selects
the original animation textures. The three phases are the six retail
`data/zasleh_front.01..03.paa` and `data/zasleh_side.01..03.paa` files.
They are ARGB4444, respectively 128×128 and 128×64. Offline image inspection
reports 20.6–35.2% partial alpha and only 4.9–12.0% opaque pixels; all six
remain translucent Blend. The ordinary M16 does not use the `fired.*` family.

Their nearly white original RGB has a green/cyan fringe. The bounded upload
helper warms only those exact virtual paths and dimensions. It uses authored
alpha as the existing dense-core/soft-lobe envelope and preserves source RGB
peak intensity, with a modest cooler-lobe reduction. Faint lobes become warm
orange; dense core becomes pale yellow-white. Every alpha byte is unchanged.
Clear texels' RGB is warmed too to prevent the old green fringe bleeding through
bilinear filtering. This creates no additional coverage or geometry. Existing
RGBA generation/mip upload and alpha classification stay in place. Other
`zasleh2`, `zasleh3`, effects, addon paths and resized sheets remain unsupported.

The direct renderer additionally requires every combined section to use an
admitted sheet and its actual animated/no-shadow/ordered-blend flags. An
already reserved material scalar, `local_specular.w=-1`, selects bounded
self-emission in `shader3d`; positive uniform wetness encodings remain separate.
The actual shader decodes sRGB once and emits at most 1.25 linear RGB per
channel. Its emission does not depend on the physical-sky sunlight, SH,
specular or local surface-light terms. Existing fog and blended-surface air
scattering still apply. The ordinary shade call remains evaluated uniformly
before the override so its implicit texture derivatives remain valid. No
renderer ABI, resource, draw, texture dimensions or smoke source is added.

Real successful `AmmoShotBullet` remains the producer. The existing weapon
proxy transform aligns the original front/side geometry; the `zasleh` animation
continues to unhide only the fired weapon for one guaranteed frame or the
existing 50 ms UI-time window, using the original three-phase random schedule.
Optics/inside-view selection, recoil, ammunition, damage and network effects
are untouched. This does not extend the visible mesh to the separate 110 ms
local-light lifetime or add persistent glow. Existing `_mGunClouds.Start(0.1)`
is retained; no new smoke appearance is claimed.

`WGR_MUZZLE_FLASH_APPEARANCE=0` restores original sheet bytes and original
material lighting in the same binary. Default is on. Startup environment must
stay fixed for each process because decoded textures and the policy are cached.
`POSEIDON_MUZZLE_FLASH_TRACE=1` reports `MUZZLE_FLASH_SHEET` with actual source,
dimensions, enabled/remapped state and alpha preservation, then one
`MUZZLE_FLASH_MATERIAL` row per distinct drawn sheet with actual dimension and
all material admission flags. These traces identify the actual visible route;
they do not replace the separate successful-shot event trace.

Verification completed in the isolated source worktree:

- Two new Catch2 cases compiled independently against the existing Catch2
  library with clang-cl: 114,733 assertions passed. These execute the production
  helper, exact phase/path/dimension admission, all material flag combinations,
  all 256 alpha values, byte-exact ablation/refusal, malformed buffers,
  preserved authored alpha, warm filtering edges and no invented black energy.
- The compiled production helper transformed all six actual decoded retail
  sheets offline: all 73,728 alpha bytes were identical before/after. Inspection
  of those helper outputs retains the authored front lobes and side plume.
- Rust parsing/formatting and `git diff --check` passed. New Naga/material-tail
  checks and a real production-helper GPU probe of 516 HDR/LDR rows are present
  for root execution; they have not been executed by the source agent.

Installed acceptance must record matching executable/DLL stamps, normal exits
and real ordinary US/M16 successful-shot events with exact proxy/sheet traces.
Capture third-person side/front and first-person/optics shots at daylight,
night and beneath an actual shelter, then the expired frame and idle weapon.
Use the same pose/camera and actual shot age for appearance ablation; random
animation phase must be recorded or matched before a pixel comparison. Check
that core reads hot, lobes remain translucent and aligned, idle shows no flash,
and burst/stance behavior and the existing local light/smoke stay correct.
Offline texture previews alone do not establish installed shader admission,
tonemapped appearance, bloom quality or temporal expiry.
