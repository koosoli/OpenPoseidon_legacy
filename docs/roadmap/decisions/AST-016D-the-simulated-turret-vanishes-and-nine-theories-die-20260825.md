# AST-016D — the simulated turret vanishes, and nine theories die measuring it

- **Ticket:** AST-016D (open defect, under AST-016/017)
- **Date:** 2026-08-25 (late session)
- **Status:** OPEN — root cause not yet named. This record exists because the
  kill-list is the asset: nine plausible mechanisms are measured dead, and the
  next session must not re-walk them.

## The defect

On the bridged A1 and A2 T-72s (`ca\tracked\t72.p3d` rev 40, `ca\tracka2\t72.p3d`
rev 48), the ENTIRE turret + commander cupola (the geometry of `otocvez` +
`otocvelitele`, ~7,300 verts on LOD 0) does not render **whenever the vehicle
is simulated with crew**. Empty/parked: renders on both renderers. Crewed:
absent on both renderers (WGPU and `--render gl33` alike). Stock OFP T72,
same crewed mission: turret perfect. The owner's reports "A2 rotor spins
wrong" and the A2 tank floating are open siblings, likely same family.

Reproduce: `dev-missions\turreta1.abel` / `turrettest.abel` (crewed EAST tank,
gunner doWatch) vs `t72drive.abel` (empty line-up). Captures in `.tmp-a1-tank\`:
`ta1/tnw/turret4/diag*/scan.png` (gone) vs `emptygl.png` (present, GL33).

## Measured dead — do not re-walk

1. **Selection synthesis (4a09d1f4) is not the cause.** The A1 tank has
   populated file selections, no synthesis lines, and vanishes identically.
2. **Vertex displacement.** Full-selection scan after `Turret::Animate`
   Transform: all 3,039 `otocvez` verts sane, Y in [-0.63 .. 2.05], zero NaN,
   zero degenerate, every frame (probe in TankTurret.cpp, temporary).
3. **AnimationSection::Hide.** Logged every Hide on the crewed tank: zasleh 1
   face + marker section 1; clan sections 12,16; svetla 13,14 — all verified
   tiny/correct against the tool's section table (turret lives in 8–11).
4. **Wrong rotation pivot.** A2 `OsaVeze` -> (0.004, -0.585, -1.277) looked
   damning until the STOCK control printed (0.036, -0.880, -1.547) — the
   engine's model space simply sits there. Axes are fine.
5. **Turret animation entirely.** `WGR_SKIP_TURRET_ANIM=1` (temporary gate in
   TankTurret.cpp) skips both Transform calls: turret still vanishes.
6. **Per-frame shape reconversion.** "Proxy model absent" x61 is one load
   (19 LODs x absent crew proxies), not a per-frame rebuild; the canonical
   adapter line appears exactly once.
7. **FindNamedSel mis-lookup.** It is a linear case-insensitive scan
   (Animation.cpp:365); no sort assumption to violate.
8. **Face index/offset unit mix.** `NamedSelection::FaceOffsets` converts
   indices to offsets lazily with bounds checks; the "mixed magnitudes" in the
   Hide log are correct offsets (zasleh face #4 -> 128; clan #~12.4k -> 400808).
9. **WGPU buffer path.** GL33 shows the identical defect, and the empty-GL33
   control shows the identical fix; no renderer-specific mechanism survives.

## The one layer not yet measured

Everything upstream of the draw call is verified correct while the pixels are
wrong on both renderers, and the flip condition is exactly "the object is
drawn through the animated path (Animate -> draw -> Deanimate) vs the static
path". The unmeasured layer is the DRAW itself: per-section face iteration /
submission for an ANIMATED adapter-built shape. Next probe: one-shot log, per
section, of the face/index counts actually submitted for `ca\tracked\t72.p3d`
LOD 0 in a crewed frame, compared against the section table (17 sections,
turret in 8–11). Whichever section's submission collapses names the bug. A
close second: `Deanimate`/`Restore` interaction with `SaveOriginalPos` on
adapter-built shapes (Restore writes OrigPos back after every drawn frame; if
OrigPos was captured wrong once, every subsequent frame re-poisons — but note
kill #2 measured POST-Transform positions sane, so this only survives if the
poisoning happens after the probe point).

## Temporary instrumentation left in tree (rate-limited or env-gated)

- `TankTurret.cpp`: InitShape axis log (WARN, unconditional — remove when
  closed), full-selection scan after body Transform (8 reports), and the
  `WGR_SKIP_TURRET_ANIM` bisection gate.
- `Animation.cpp`: blow-up probe in `Animation::Transform` (16 reports),
  Hide content log (24 reports).

All are load-bearing for the next session and cheap; remove them in the fix
commit, not before.
