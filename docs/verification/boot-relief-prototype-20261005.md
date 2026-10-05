# Authored boot relief prototype

Start with `POSEIDON_BOOT_RELIEF_PROTOTYPE=1` to enable this bounded WGPU prototype.
The normal game path stays unchanged without the flag. No new assets are required.

## Admission and visual treatment

The actual player boot producer tags a mark only after a mud or sand imprint changed
the corresponding physical field. The renderer additionally requires an exact
`Mark` object, the preloaded `FootStepL` or `FootStepR` shape, an on-ground alpha
material, unskinned geometry and a read-only depth test. Other marks, dry/rejected
contacts and snow keep their existing rendering. Existing surface refusal and
footstep lifetime rules remain in effect; saturation can therefore refuse a new
prototype mark.

The authored texture's alpha and darkness infer a depression up to 3 mm. Its local
gradient supplies a lighting normal; a single bounded UV offset provides near-view
parallax. UV displacement is capped at 0.02 and detail fades from 4 to 12 metres.
The ordinary mark alpha and lifetime remain authoritative.

This shades the existing receiver colour instead of painting the dark source
texture over the ground. With receiver colour `D = S + T * ground`, multiplier
`M <= 1` and fog airlight `S`, the dedicated blend computes
`S * (1 - M) + M * D = S + M * T * ground`. Framebuffer alpha is preserved.
The airlight uses the existing terrain-fog helper. This prevents multiplying the
foreground fog into a dark boot-shaped patch, assuming the mark and receiver share
the same ground position. Restricting the multiplier to at most one also avoids
negative airlight compensation on an LDR target.

This is inferred visual relief, not a measured sole height map or new collision
geometry. The normal cue uses the existing sun lighting; it does not add a full
local-light material model. It does not extend tyre physics or the elevated snow
surface contract.

## Checks completed

- `scripts/Test-BootRelief.ps1`: actual composed renderer shader validation,
  headless GPU height/normal/parallax/fade/receiver-colour and fog compensation
  assertions, blend state and material flag regression.
- `scripts/Test-PlayerGroundBoot.ps1`: player ground events, mud and sand physical
  admission, field and producer contract checks.
- Configured the isolated worktree and compiled the affected `SoldierOldMove.cpp`
  and `EngineWgpu.cpp` objects plus the Rust renderer library successfully.

## Runtime fixture for integration

The integrating agent still owns installed-game verification; this prototype has
not been deployed or launched by its author. On original Everon or Nogova, use
open wet mud/sand and walk a fresh strip. `POSEIDON_PLAYER_GROUND_TRACE=1` can
confirm a changed mud/sand contact. A first accepted draw writes
`BOOT_RELIEF_PROTOTYPE admitted=authored-footstep-mark` to the log.

Inspect within the existing ten-second mark lifetime, from roughly 1–3 metres
and an oblique view. Compare the flag enabled/disabled on the same ground, then
repeat in strong fog. Ground colour should remain recognizable; no black source
texture, rectangular fog darkening or unstable parallax edge should appear.
Roads, roofs, sea, dry ground and snow should retain the previous behaviour.

A synthetic harness can spawn an actual preloaded FootStep mark with a longer
lifetime and set its relief tag after an admitted physical mud stamp. That is
useful for repeatable camera inspection but does not substitute for the actual
player-contact test.
