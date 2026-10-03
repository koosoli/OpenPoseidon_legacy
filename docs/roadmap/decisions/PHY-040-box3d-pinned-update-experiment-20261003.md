# PHY-040: isolated Box3D dependency update experiment

This is an opt-in dependency experiment, not a shipping update or completed
ragdoll acceptance. The shipping `vcpkg.json`, package-only registry baseline
and existing overlay directory are unchanged. Root owns full engine builds,
installed comparisons and deployment. This candidate changes no renderer or
gameplay death policy.

## Actual dependency and upstream audit

The installed control's package manifest/SPDX identifies Box3D `0.1.0`, source
`erincatto/box3d@v0.1.0`, SHA512
`92eaa76bb0e309d10ae2f408183487e5c55eeff71428ae1a0d1b814d892ac532aca10069414c4b0ef998a754f0bf420a41838c94d70073485f6f9260371aa737`.
The exact release tag is `8441b4a06d6d09dcfb0b0f704df4d847d1437b92`.
It resolves through package-only vcpkg baseline
`d87340acc46bdeda386037b38aca30136e667e47`, leaving unrelated libraries pinned.
The upstream author labels the [initial release](https://github.com/erincatto/box3d/releases/tag/v0.1.0)
alpha software; live official release/API checks on 2026-10-03 still find only
that release. Current upstream main was
`51f056e0a9d299326b10f10a63270c81d901df21` (2026-10-03T03:47:05Z), thirty commits
after the release. Both main's CMake version and current vcpkg still say 0.1.0;
the version string alone cannot identify the newer implementation.

Relevant official changes include:

- [Fixes #40](https://github.com/erincatto/box3d/pull/40), commit
  `655799893a9d79900a59e863b880e5e3f7e53593`: `SetMassData` refreshes the world
  inverse inertia and extents, and the newer implementation manages dirty mass.
  Our `SpawnArticulatedPiece` calls the existing `SpawnDynamicPieces`, which
  rescales convex mass/inertia through this exact API before applying impulses.
- [Ghost collision improvements #61](https://github.com/erincatto/box3d/pull/61)
  and [Fixes #97](https://github.com/erincatto/box3d/pull/97): triangle/hull
  contacts and a hull stacking stability regression.
- [Fixes #98](https://github.com/erincatto/box3d/pull/98): hull builder face/edge
  leaks, relevant when corpse pieces are repeatedly created and retired.
- [Fixes #157](https://github.com/erincatto/box3d/pull/157), commit
  `954cf879e717334eb96da9a5c255788fdc9f5f87`: existing contacts are removed on
  creation of joints that suppress connected-body collisions; contact recycling
  after mass-center changes, compound queries and broad-phase cleanup improve.

The chosen experiment pins `954cf87`, immediately before the broad-phase rewrite.
[Open upstream issue #164](https://github.com/erincatto/box3d/issues/164) reports
force-driven bodies freezing after the next commit `f555ee4` in another game's
many-body world. The maintainer requested a recording and questioned that game's
use of bullet flags on every body. This is a reported risk, not a diagnosis of
our world, and not evidence that current main still reproduces it. Evaluate main
separately with actual multi-body terrain scenarios before promoting it.

## Controlled mass and impulse evidence

`tests/experiments/box3d_mass_update.c` is an identical library-only fixture on
both sides: a convex octahedron whose actual volume is one sixth of its bounding
box, mass rescaled to 4 kg, quarter-turn orientation and a real off-center linear
impulse. A closed-form uniform-volume inertia gives an independent oracle.

| Actual measurement | Released control | Pinned 954cf87 | Expected |
| --- | ---: | ---: | ---: |
| Requested mass, kg | 4 | 4 | 4 |
| Immediate world X inverse inertia | 115.384575 | 19.2307606 | 19.2307682 |
| Off-center impulse angular X velocity | -23.0769157 | -3.84615207 | -3.84615374 |
| Mass after density-only edit and step, kg | 4 | 4 | 4 |

The control fails the inertia and impulse assertions; the pin passes all four.
This establishes a stale world-inertia cache in the actual released mass-rescale
path. It does not claim that the released version had the later dirty-mass flag:
the density preservation control passes on both versions.

`tests/experiments/box3d_backend_mass_update.cpp` additionally calls the real
engine seam: `SpawnArticulatedPiece`, `ApplyImpulse` and `GetBodyMotion`.
The 4 kg octahedron should receive angular Y velocity -2.5 rad/s. Released control
fails with -15.00001; pinned code passes within .005 rad/s. Both preserve the
correct linear velocity and ownership cleanup. This experiment is deliberately
outside the default unit target while the released dependency remains shipping.
The same file records thirty real backend solver steps and checks actual replay
frame count, nonzero byte count and no divergence.

Existing compiled physics, articulation and affine-corpse fixtures pass on both
libraries: **1563 assertions / 43 cases**. They include terrain replacement and
tile seams, body/joint cleanup, replay-independent deterministic runs, worker
readback and authored affine preservation. The new actual engine mass fixture
passes **7 assertions / 1 case** on the pin and fails one on control. The replay
fixture passes **11 assertions / 1 case** on both versions. These focused native
checks do not accept installed corpse skin/contact appearance.

Private logs/executables are in the isolated worktree's ignored
`build/box3d-pinned-experiment/`: `mass-{control,pin}.log`,
`{control,pin}-{existing,mass,replay}.log`, corresponding compile/link logs and
`{control,pin}-physics-tests.exe`. Production `Box3DBackend.cpp` was rebuilt from
this worktree against each library's own headers. The small executable links
unchanged support libraries from root's full build; affected physics/pose units
and tests are separately rebuilt. It is not a full engine build.

## Opt-in package and API migration

`cmake/vcpkg-experiment-ports/box3d` downloads only exact `954cf87`, verifies
SHA512 `73e8da5429a14b99401a7bb2bcb6e781446f3f9c6db7dfd9b9caeb27cebbec1825513c8dfaa41b8cca6da819f7a8afe812090c788f74d56478f0f3d48f563125`,
and retains vcpkg's CRT and x86 popcount patches. It exports
`POSEIDON_BOX3D_PINNED_954CF87=1` through the package target. Only under that exact
definition the private backend uses renamed `GetWorldCenter`, `b3CreatePlayer`
and `b3DestroyPlayer`; default compilation still uses the release API.
The backend name includes `+954cf87` for a visible runtime witness, and the
optional corpse runner accepts that exact suffix in addition to the release.
The upstream binary structs, allocator callbacks, compound layout and recording
format changed despite the unchanged 0.1.0 version. Rebuild library and every
consumer together; old recorded bytes are not a cross-version compatibility
contract (released record format 2.2, pin 6.4). Preserve control recordings.

The actual overlay package builds Debug/Release and passes vcpkg post-build
validation in the isolated install tree. Its exported CMake target carries the
revision definition; an independent CMake consumer checks that property and
passes the C oracle. Relinking the actual backend fixtures with the overlay's
installed library also passes all existing 1563 assertions and both new cases.
Control release library SHA256 is
`f69e20d10d0a2ae4e9ac7fb9e48a13ea9407b7d8f0a610f5654d709c0b18c1eb`;
experimental overlay release library SHA256 is
`255fd380f28be2fd51efe9c9e01dedec88acab930ef1487b4a0a2f6768c70431`.
Focused test command extraction emitted warnings for two unused external-data
path macros; selected physics/affine fixtures do not consume those macros. Full
native target compilation remains root's separate gate.

`scripts/Test-Box3DMassUpdate.ps1 -ControlInclude <release/include> -ControlLibrary
<release/lib/box3d.lib> -PinInclude <experiment/include> -PinLibrary
<experiment/lib/box3d.lib>` builds the identical C fixture against explicitly
provided libraries, requires the precise control failure and pin success, and
records source/header/library/executable identities and outputs. It launches no
game, changes no dependency and performs no deployment.

Select the experiment only in a separate engine build directory:

```powershell
cmake --preset win-x64-clang-rwdi -B build/win-x64-clang-box3d-954cf87 -DVCPKG_OVERLAY_PORTS="${PWD}/cmake/vcpkg-experiment-ports;${PWD}/cmake/vcpkg-overlay-ports"
```

The second overlay keeps the existing OpenAL fixes. No registry-wide baseline
bump is needed. Omitting this override retains the shipped release recipe.
The first library-only build failed because CMake could not locate the Windows
manifest tool; configuring its actual SDK path resolved it. The first private
vcpkg fetch hit a TLS transport error; the independently checksum-verified exact
GitHub archive was then seeded into the private download cache. Neither event
changed root's installed dependencies or game files.

Promotion still requires root's full native build/tests and controlled installed
comparisons, actual stock contact fitting and multi-body terrain/replay checks.
The current initial corpse fit refusal precedes body creation and is independent
of this confirmed impulse defect. Keep automatic deaths disabled while those
gates remain open.
