# REN-GI-005 — A room with a window: the shaded wall comes up 35%, the sunlit wall does not move

**Date:** 2026-09-03
**Branch:** `asset-obs-001`
**Closes:** the first "not done" of REN-GI-003 ("a room with a window rather than a barn door")

## The pose, and why it is built rather than found

REN-GI-003 judged interiors on the open barn: a roof, an open side, no window. A window is the
harder and more common case — a small bright opening in a large dark room.

The pose **places its own building**: `Dum09` (`\O\Hous\zluty_statek_in`, the enterable
farmhouse) is put on the open hill at Everon 7050 / 6500 by the mission itself, with the player
standing inside it. `gi_house0/90/180/270.eden`; **270 is the acceptance view** — a window on the
left throwing sunlight onto the near wall, the far interior wall in shade on the right.

Placing it is deliberate. The remaster's Everon does **not** use the object coordinates that
`Noe.pbo`'s WRP lists: a church the WRP puts at 5442.9 / 9485.6 is not there under either that
coordinate or its mirror, checked by capture. A pose keyed to those coordinates cannot be
reproduced, so the mission carries the building. It needs `addOns[]={"bis_resistance"}` — every
`Dum*` class is datadisk, and without the declaration the mission does not boot at all
("Cannot load mission, missing addons").

## Measured (Everon, noon, 3441×1440, `POSEIDON_VSYNC=0`, two runs per arm)

| region | GI on | GI off | |
|---|---:|---:|---|
| **interior wall in shade** (right of frame, above the floor) | **56.02** | **41.41** | **+35.3%** |
| **sunlit wall beside the window** (left of frame) | 112.72 | 113.90 | **−1.0%** |

Both arms repeat to within 0.03 luma across runs, so those figures are the change and not the
spread.

This is the behaviour the probe volume exists for, stated as plainly as it can be: **light that
came through the window reaches the wall that cannot see the window, and the surface the sun
already lights does not change.** The barn pose could only show +3% (17.6 → 18.1) because a
doorway lights the whole room at once.

## Cost

| | |
|---|---:|
| `GI: probe update` | 0.19 ms |
| the gather, i.e. `[container] 3D colour sub-pass` on vs off | 4.20 vs 3.95 → **+0.25 ms** |

**The frame total is not the instrument on this pose** and is reported here so nobody quotes it:
the two GI-off runs measured 15.68 and 19.05 ms, a 3.4 ms spread inside one arm, while the
colour sub-pass held 3.94 / 3.96. A first reading of the frame totals said GI cost 3.7 ms. It
does not; it costs about 0.45.

## A separate bug this pose exposed

**Grass grows through the floor of a mission-placed building.** It is not there in a building
the world itself places (the barn pose's floor is clean), so the grass mask is fed by world
placement and never learns about objects a mission adds. It is cosmetic here and it polluted the
first luma reading until the measurement box was moved above it. Filed separately; nothing in
this decision changes it.

## Still not done

The sun-bounce gain default (`WGR_GI_RSM_GAIN`) still has not been chosen against a pose with
deep shade beside a sunlit wall — this room is lit through a window, which is a different case.
