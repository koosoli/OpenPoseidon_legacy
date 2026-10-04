# Automatic stock infantry death candidate, 2026-10-04

The existing installed explicit stock-corpse articulation, measured elbow/knee
hinges and owned freeze remain the control. This candidate adds an independent
ordinary damage-death path. It is opt-in with `POSEIDON_AUTOMATIC_RAGDOLL=1` until
the runtime owner accepts the installed ordinary-death campaign. Source or CPU
success is not an installed appearance, save/load or gameplay acceptance.

The developer panel's Physics tab provides **Automatic infantry deaths
(experimental)**. With developer mode active (`--dev`), Ctrl+F8 opens the panel.
The checkbox explicitly overrides startup admission until restart. Disabling
stops new ragdolls and preserves every existing active/frozen owner. The harness
uses `corpse-auto-enable`/`corpse-auto-disable` through the same setter; the
`-RuntimeToggle` runner starts disabled, proves activation, checks full active and
frozen pose retention when disabled, and proves a new disabled death remains
authored even when active capacity is free. The startup default remains off.

## Supported candidate boundary

- Local single player, locally owned `data3d\mc vojakw2.p3d` stock Western
  infantry, alive in the landscape before the new damage transition.
- Dry, unattached, non-ladder, visible ground contact. Initial vertices must agree
  with the actual registered Box3D roadway floor within 25 mm. Non-boot parts
  remain at least 3 mm above it; only stock lower-leg/boot parts 8 and 10 permit
  up to 25 mm initial overlap for the solver to resolve. Open-ground legacy center
  rays and measured shared anchors remain
  admission gates. The solver has no certified cave/trench cavity support.
- Four independently owned active corpses, distinct negative self-collision
  families, up to 64 retained snapshots. Excess, ambiguous or unsupported deaths
  follow the existing authored death. No earlier corpse is cleared to admit one.
- Eleven authored component bodies and ten joints, including four measured
  anatomical hinges. Production uses 90% of each actual component about its
  centroid, with the existing bounded 10 mm contact-proxy calibration; the
  explicit settled-corpse control retains its 40% proxy fit.
- Inherited actual linear/angular motion and the existing queued gameplay
  impulse are consumed once. Initial speeds above 15 m/s or angular velocities
  above 20 rad/s refuse this bounded candidate. It is not multiplayer ragdoll
  replication, dismemberment or a full high-energy blast simulation.

## Consumer and ownership changes

`CorrectedCorpsePose` captures actual post-correction render palettes, CPU points
and equipment proxy matrices separately. Head/aiming/leg correction arithmetic
differs between these consumers; raw phases are not substituted for the final
outputs. Each admitted capture compares all available actual consumers again
with the owned snapshot bypassed and requires exact zero-motion bytes. Reserved
palette-only face points retain their palette path. Production moves the
captured outputs using body deltas without accumulating earlier simulation
errors; original diagnostic raw affine phases remain unchanged.

The first `ReactToDammage` / `KilledBy` damage death queues capture before authored
death movement advances. Admission is consumed at the beginning of `Man::Simulate`.
The entity remains dead for gameplay while its original animation identifiers
remain frozen. Physics updates the limbs and rebases the object's spatial root
from pelvis displacement; bounds include actual render and CPU points. Existing
death callbacks, damage/scoring, the blood callback and mission-script controls
remain on their existing paths.

The latest validated pose freezes after 0.75 seconds of quiet motion or a bounded
12 simulated seconds. Joints are released before bodies. Handle-free pose data
persists beyond the diagnostic's old 30-second lease. Body readback or bounds
failure retires at the last valid pose. Hide, deletion, revival, external movement,
world/model or ownership changes invalidate it. This changes no ordinary living
infantry integration or diagnostic expiry policy.

The automatic path reuses registered shared terrain and never replaces a live
floor. Only a missing floor is initialized from actual map heights/spacing, with
at most four million samples; support disagreement still refuses admission.
Loose objects, probes and their handles remain independently owned.

## Frozen save data

Saving an active admitted corpse freezes its latest validated pose. The optional
`opAutomaticCorpse` version-1 subclass stores bounded palettes, CPU points,
equipment matrices, root and bounds, plus exact model/map and bone names. It stores
no pointers, epochs, body/joint handles or active solver state. Load rebuilds
bindings from the actual model, validates counts, names, finite data, proxy
membership, root and recomputed bounds, then restores an owned frozen corpse.
Unsupported or absent data retains authored fallback. No network representation
is added. Runtime round-trip remains mandatory before promotion.

## Verification and remaining acceptance

Private actual backend/native checks passed: 2,302 assertions in four measured
hinge cases and 2,871 assertions in 19 corpse/articulation/mass/replay cases,
including the new corrected snapshot and collision-family/motion controls.
Actual changed backend/world/infantry/Sim/SimProxy translation units passed
syntax checks. The complete game build, ordinary rifle deaths, source skin contact,
save/load, multiple corpses, near/far culling, lifecycle and performance require
the matching installed runner and the runtime owner's review. Default promotion
is explicitly pending those receipts.

### First installed standing admission, 2026-10-04

The matching `cdf0dba3` Everon campaign captured four ordinary stock standing
deaths with exact actual zero-motion outputs (363 palettes, 6,394 CPU points and
37 equipment proxies per owner); the fifth death took the budget fallback.
Admission then refused all four. This is a failed installed campaign, not a
completed ragdoll feature. The initial caller sampled the 50 m LAND grid at four
subdivisions (12.5 m) while the actual terrain grid is 6.25 m, losing real vertices
and causing approximately 0.26--0.29 m solver/native floor disagreement.

The correction registers actual terrain-grid vertices, including the closing
row/column rather than `SurfaceY`'s outside-map return. The ceiling is exactly
4,194,304 samples, admitting a 2048 by 2048 map; larger maps refuse instead of
downsampling. Native backend nonplanar-cell ray tests distinguish the actual
01--10 diagonal and intermediate ridge from coarse/bilinear approximations.
The existing 25 mm floor agreement and 3 mm vertex-clearance gates remain.
Small solver quantisation differences participate in the existing 10 mm maximum
contact-proxy calibration without moving the original skin, origin or anchors.

Two captured standing owners also showed genuine native lower-leg proxy
penetrations of 8.53 mm and 16.95 mm at the 90% component fit, independently of
the incorrect solver floor. Those exceed the calibrated offset budget once
clearance is included and remain unsupported; the floor correction does not
claim to fix authored stance or visible skin contact. A matching rerun and
measured supported-ground/foot-fit evidence remain required.

The matching corrected-floor `cdb1977b` rerun registered all 2048 by 2048 native
vertices at 6.25 m (16,777,216 source bytes) in 246.7666 ms. Owners 0 and 2 entered
the actual eleven-body/ten-joint/four-hinge solver; owners 1 and 3 still refused
their native boot contact. This identifies original contact separately from
floor approximation. The first-death setup hitch is a material default-promotion
limitation; the caller reuses the shared floor afterwards, but no loading-stage
preparation or broad-map performance acceptance is claimed here.

The next contact candidate admits only those two stock lower-leg/boot parts with
at most 25 mm overlap against both agreeing floors. The original captured pose,
body origins, skin and anchors do not move before stepping. All other parts keep
3 mm clearance; greater boot penetration and floor disagreement still refuse.
The actual Box3D contact-response control covers the observed 8.53/16.95 mm starts
and the 25 mm boundary with a connected hinged chain on real terrain: zero initial
transform change, finite bounded velocities, recovered distal contact, preserved
anchors and zero handles after removal. This uses controlled hulls to test solver
response, not a substitute for the actual stock skin/rig installed campaign.

Matching installed `3de356cb` campaigns now pass four actual standing deaths
(44 bodies/40 joints), physical falling/root rebase, persistent frozen outputs
after 31.25 simulated seconds, exact full palettes/CPU points/equipment/bounds and
delete cleanup with zero handles. A separate mission save/load campaign passes
validated frozen restoration and zero solver handles. Both exit normally; the
runtime owner inspected all four visible corpses with contact/shadows and found
no obvious disappearance or stretched rig in this bounded fixture. This is
retail Everon stock Western infantry evidence, not broad-model/map coverage.

The first enemy rifle fixture did not kill its target within 45 seconds. That
campaign therefore fails the rifle gate; it does not demonstrate a ragdoll
failure. The revised bounded fixture faces a stock Eastern shooter toward the
target, gives only an actual AK74, waits for targeting and issues ordinary
`doTarget`/`doFire` plus the existing script `fire` command. It records actual
Fired weapon/count, ammo, knowledge, positions and target damage before timeout.
Only a genuine AK74 projectile death can pass; it never substitutes setDammage.
The installed `7d5c1e92` runtime-toggle campaign passes: shared setter activation
from startup-off, full active/frozen retention on disabling, an authored new
disabled death while capacity is free, re-enabling and final cleanup. The revised
rifle fixture still failed before producing any valid shooter witness. Actual
receipt placeholders and the sole WEST centre exposed the cause: `createGroup
east` returns null without an EAST centre. The fixture now calls `createCenter
east` first, initialises the shooter to objNull and independently proves its live
stock type, position, actual AK74 and ammunition before collecting arrays. Rifle
acceptance was subsequently established in matching installed `7d5c1e92`: the
verified live Eastern AK74 shooter fired five actual shots, killed the stock
Western target, and produced the fifth automatic frozen corpse. The runtime
owner inspected that retained corpse; normal shutdown and deletion cleanup
passed with zero solver handles. No artificial damage command was used in the
rifle arm. See `docs/verification/automatic-ragdoll-20261004.md` and its curated
runtime receipt for the exact author-run evidence.

The requested MIDDLE campaign is not crouched acceptance: its actual initial CPU
consumer points match the UP fixture exactly. Requested stance does not prove the
animation changed. Actual crouch/prone deaths remain unverified; no forced pose or
additional production change is introduced to close that gap.

### Owner airborne blast correction

The owner reported soldiers flying upward and remaining suspended. Source audit
found an unconditional freeze whenever any part travelled more than 12 m, plus
an unconditional 12-second retirement and invalid-readback/bounds retirement.
Those were prototype safety bounds, not evidence of a supported frozen pose.
They can stop a perfectly finite ballistic body in the air.

Automatic retirement now requires actual Box3D static-solid contact with an
upward normal above 0.5, a contact separation at most 15 mm and current native
road/terrain agreement within 25 mm. Side-wall contact is not ground support.
Travel from the initial world position no longer retires an otherwise valid
rebased skin/rig. Both quiet and time retirement require low motion and support;
queued impulses run before retirement. Explicit automatic freeze and save-freeze
also refuse unsupported or fast bodies. The manual settled-corpse control stays
on its existing explicit policy.

An invalid body, affine output or bounds now releases its handles and returns to
authored falling with the last finite pelvis velocity/angular velocity, current
validated root and ground-contact reset. Saving an unsupported active corpse
stores its actual ordinary entity falling motion and omits a frozen snapshot;
the loaded unsupported case uses authored falling. Frozen supported round trips
retain the previous format. This is not new active-solver save serialization.

The real Box3D regression crosses the old 12 m cutoff while airborne with an
8 m/s horizontal and 10 m/s upward inherited velocity. It refuses retirement
even past the old time limit, then recovers actual floor contact and low motion.
It separately rejects a side wall and stale handles. Existing contact/hinge/
affine/native-floor checks remain passing; matching installed trajectory, actual
grenade death and standing/save/control regression remain required. Initial
motion above the existing 15 m/s or 20 rad/s admission still uses authored
fallback, and excess active-owner budget remains authored fallback.

Focused current-source native checks passed 12,173 assertions in 22 cases,
including the actual airborne floor/wall solver regression; the measured hinge
subset passed 2,302 assertions in four cases. The four modified production
translation units pass syntax checks with the ordinary preset's arguments.
`Test-AutomaticRagdoll.ps1 -InheritedBlastTrajectory` records inherited motion,
the old travel cutoff, explicit freeze refusal and a save that keeps live
handles. `-BlastDeath` separately requires a real `GrenadeHand` explosion to
kill an actual stock soldier and records damage/death and ground recovery.
Neither arm is installed acceptance yet; unsupported airborne save reload is
also unexercised by this candidate runner.

### Frozen hit reaction and earlier terrain preparation candidate

The frozen update previously returned before consuming real incoming projectile
or explosion impulses. Each admitted runtime corpse now retains its original
local convex hulls, masses, joint frames and last validated solver transforms.
A real pending impulse can rebuild the same eleven bodies and ten joints from
those transforms, with one available collision family out of the existing four.
The retained palettes, CPU points, equipment proxies and root remain unchanged
before the next solver step. Continued deformation starts from that actual
frozen output, rather than an authored standing/death animation.

Wake refuses changed world generation or terrain serial, missing recipes,
incompatible entity/model ownership, hidden bodies, wet support and exhausted
active budgets. Refusal keeps the frozen pose and clears the refused impulse.
Legacy handle-free loaded snapshots contain no solver recipe and remain frozen;
this candidate does not silently reinterpret their saved skin as a new rig.

Real aggregate force/torque identifies an impact line. The closest admitted
anatomical part receives the original impulse scaled only by the existing
`Rigid()` material response. Actual post-impulse velocity readback enforces the
existing motion bounds; larger safe impulses use the previous aggregate path.
No synthetic damage or force amplifier is introduced. Read-only hit counters
distinguish an issued projectile from actual received impulses, wakes and local
transfers. The initial pending readback excludes the death impulse already
included in spawn motion; it cannot apply that same impulse a second time.
Later independent hits can wake the same supported corpse again.

The existing native terrain registration runs once when enabling the feature,
or during the first live infantry simulation on an enabled mission. A cached
world/landscape/generation/terrain-serial identity prevents repeated per-unit
preparation and repeated attempts after failure. Setup still has its measured
cost; it moves before combat rather than disappearing. Existing registered
terrain is preserved. The runner requires preparation before fixture damage
and proves the first death does not register terrain again.

Focused actual Box3D checks pass 12,295 assertions in 23 cases, including the
new rotated eleven-body recipe, real local impulse, exact retained affine
consumer and stale-terrain/world controls. Hinge checks pass 2,302 assertions
in four cases. Installed acceptance is pending. Run
`Test-AutomaticRagdoll.ps1 -FrozenProjectile` against the matching installed
pair: it sends real stock `BulletSingleW` ammo through measured frozen chest
Fire geometry, requires confirmed incoming impulse/wake/local transfer, changed
consumed pose and supported resettlement with zero handles. This is native ammo
collision coverage, not a player input or `Fired` event acceptance claim.
